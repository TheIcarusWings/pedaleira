# Pedaleira command reference

Generated from the command registry. Do not edit by hand: a test compares this file
with the registry and fails when they differ.
Regenerate with `PedaleiraRender --describe-markdown > docs/commands.md`.

A request names a command and gives its arguments as a JSON object:

```json
{"command": "block.setParam", "args": {"blockId": "...", "param": "mix", "value": 0.5}}
```

Every command answers with one of two envelopes:

```json
{"ok": true, "result": {}}
{"ok": false, "error": {"code": "invalid_args", "message": "..."}}
```

Error codes:

| Code | Meaning |
|---|---|
| `unknown_command` | No command of that name exists. |
| `invalid_args` | An argument is missing, unknown, of the wrong type or outside its allowed values. Nothing was changed. |
| `not_found` | A block, preset, mapping or file named by the arguments does not exist. |
| `failed` | The arguments were valid but the engine refused or could not do it. |
| `unavailable` | The command could not start or finish in time. Trying again later can succeed. |
| `permission_denied` | Command server clients only: the command's permission class is switched off, the command is local only, or a path is outside the allowed folders. |
| `needs_approval` | Command server clients only: the command changes the rig being played and waits for the user. The error carries `approvalId`; poll `approvals.status`. |

Each command has a permission class, which decides whether a command server client (an
agent) may run it: `read`, `edit`, `render`, `live`, `delete`, `settings` or `local` (never
available to clients). See docs/agents.md.

Arguments are validated before a command runs. An optional argument may be left out or
given as `null`.

## Commands

| Command | Mutates | Permission | Summary |
|---|---|---|---|
| [`agents.activity`](#agentsactivity) | no | local | Returns the most recent commands that came through the agent layer, newest last (the last 200 are kept): time, client, command, its arguments shortened to one line, the rig it acted on and how it ended (ok, an error code, or running). |
| [`agents.disconnect`](#agentsdisconnect) | yes | local | Disconnects a client of the command server; its sandbox rigs are discarded and its requests waiting for approval are denied. |
| [`agents.list`](#agentslist) | no | local | Lists the clients connected to the command server: id, the name each declared, when it connected, its last command and when, how many commands it has run and the rig it is working on (live or a sandbox id); plus the server's status. |
| [`approvals.list`](#approvalslist) | no | local | Lists the requests waiting for the user's approval (a client asked to change the rig being played while that is switched off), each with its id, client, command and arguments; with all true also the recently resolved ones. A request to apply a sandbox carries applies: the sandbox, the revision it was captured at, when, and sandboxChanged (the sandbox is no longer the rig the request would apply, so a preview rendered through it would show something else). |
| [`approvals.resolve`](#approvalsresolve) | yes | local | Approves or denies a waiting request. Approving runs the command exactly as it was asked for and keeps its result for the client, which reads it with approvals.status. A request to apply a sandbox (sandbox.applyToLive) applies the rig the sandbox was when the request was made (applies: its sandbox, revision and capture time), whatever has been done to the sandbox since. |
| [`approvals.status`](#approvalsstatus) | no | read | Returns one approval request: state (pending, approved, denied or expired) and, once approved, result, the envelope the command answered with. A client sees only its own requests. |
| [`assets.list`](#assetslist) | no | read | Lists the amp captures (.nam) and impulse responses (.wav) an agent can load with chain.addToneFromFiles: the files under the library folder and the folders the user added (source library, tags from the folder names), the library's asset store (source assets: the model files of the saved presets, under content-hash names) and the local model stash (source stash: files added before, also under content-hash names). origin says where a file came from: library, tone3000 (downloaded on the TONE3000 page, kept in the library's TONE3000 folder; listed with tone3000, the tone, creator and licence), folder (one the user added), or loaded (a copy kept for a file the user loaded). A copy kept under a hash name is listed by the name the file was loaded under (name, with loadedFrom and loadedAt, and tone3000 with the tone, creator and licence when it was downloaded from TONE3000), else by its metadata name, and query finds it by that name. A capture carries what its file says about itself as nam_name, nam_modeled_by, nam_gear_type, nam_gear_make, nam_gear_model, nam_tone_type, nam_gain and nam_loudness when present, and its architecture, architectureLabel, a2, sampleRate (when the file names the rate it was trained at) and supported (every architecture NAM builds loads; false for a file Pedaleira can not load, with unsupportedReason in a sentence); an IR its sampleRate, channels and seconds. inRig lists the tones the rig already holds, with their titles and gear tags. There is no searchable catalogue beyond this: tones from the online catalogue are only known once they are in a rig. |
| [`assets.tag`](#assetstag) | yes | edit | Adds tags to a capture (.nam) or IR (.wav) and removes others, in the user's own index (asset-tags.json in the library folder). Tags are kept by the file's content, so they follow it through a move or a rename. A tag is lowercased, trimmed and at most 32 characters; a file carries at most 32. Returns the file's tags after the change. See assets.tagSummary for the tags in use and a suggested vocabulary. |
| [`assets.tagSummary`](#assetstagsummary) | no | read | How far the user's captures and IRs are tagged (captures and irs: total and tagged, over the same files assets.list finds), every tag in use with how many files carry it, and a suggested vocabulary to keep tags consistent (gain, character, gear, speaker, use). Agents choose tones by these tags. |
| [`block.exportImpulse`](#blockexportimpulse) | yes | edit | Writes the impulse response a cab.model block plays (docs/cab-framework.md) as a mono WAV file, so the cab can be used as an IR anywhere: the response its settings give now (speaker, box, mic, position, distance, angle, room and cuts; not its level), computed at the sample rate asked for, about 85 ms long. 16 and 24 bit files are scaled so their peak is at -1 dBFS (the answer's gainDb says by how much); a 32 bit float file keeps the level the block plays at. Only blocks of a kind that synthesises its own response accept this (cab.model). |
| [`block.getFaceplate`](#blockgetfaceplate) | no | read | How a block's controls are drawn as a faceplate, for the blocks that have one (the amp kinds; for any other kind the answer is invalid_args). It is the panel style of the block's amp description (docs/amp-framework.md, The faceplate), with the defaults filled in when the description gave none (styled false). Returns the amp's name, family (High gain, Clean, Crunch, or empty), tag and character line, the accent colour as #RRGGBB (empty: the front end's own), the nameplate position (left, right, top, bottom), the knob cap (round, pointer, chicken_head, fader), the strip pattern, the voicing names, and the groups: the boxes left to right, each with a title, its controls in order (parameter ids from block.listParams: gain, bass, middle, treble, presence, depth, master, bright, voicing, each with the label the description gave it) and the columns for its knobs (0: one per knob). Every control the amp's panel has is in exactly one group. Level is not in a group: it is the block's output knob. |
| [`block.listParams`](#blocklistparams) | no | read | Lists everything block.setParam can set on a block, with current values: the five common settings (scope common, knob positions from 0 to 1) and then the parameters of the block's kind (scope kind, real units), each with its range, unit, default and flags. A hosted plugin block lists the plugin's own automatable parameters (the first 256) as normalised 0 to 1 values under the plugin's parameter ids, with the plugin's display text for the current value in text. |
| [`block.loadDescription`](#blockloaddescription) | yes | edit | Gives an amp.custom block an amp: loads an amp description file (JSON, format pedaleira-amp, see docs/amp-framework.md and docs/amp-description.schema.json) that lists the amp's parts as read off its schematic, and builds that amp. The file is checked first; if it is refused the answer is invalid_args with the loader's message, which names the field, and the block is left as it was. Otherwise the block takes the amp's name, its voicing is set to the description's default, and the description's text is kept in the block's state (saved presets do not depend on the file staying where it was). One undo step. A new amp.custom block starts as the shipped High Gain amp. A cab.model block takes a speaker description instead (format pedaleira-cab-speaker, see docs/cab-framework.md), which becomes its Custom speaker and is selected. Only blocks of a kind that takes a description (amp.custom, cab.model) accept a file. |
| [`block.resetEq`](#blockreseteq) | yes | edit | Returns a block's EQ to its flat defaults, switched on and post-model. |
| [`block.setEnabled`](#blocksetenabled) | yes | edit | Turns a block on or off (off passes the signal through untouched). A block of a kind with spillover (see kinds.list) whose spillover parameter is on keeps ringing after it is turned off: its input closes and its tail plays out over the dry signal, for as long as the block's reported tail (at most 30 seconds); with the parameter off the tail is cut. The state belongs to the active scene: a block is in the same state in every scene until this is first used on it, and from then on each scene has its own. |
| [`block.setEqBand`](#blockseteqband) | yes | edit | Changes one band of a block's six-band EQ; fields left out keep their current value. |
| [`block.setEqEnabled`](#blockseteqenabled) | yes | edit | Turns a block's EQ on or off, keeping its band settings. |
| [`block.setEqPre`](#blockseteqpre) | yes | edit | Places a block's EQ before its model (true) or after it (false, the default). |
| [`block.setParam`](#blocksetparam) | yes | edit | Sets one of a block's settings. The common ones are knob positions: inputGain, outputGain and mix from 0 to 1 (0.5 is unity for the gains), enabled and normalize as switches (0 or 1). Any other id is a parameter of the block's kind (see block.listParams) and takes a value in that parameter's own unit, clamped to its range (0 to 1 for a hosted plugin's parameters). The blocks carry the active scene: enabled and a scene-assigned parameter (scene.assignParam) change in the active scene only. While the preset has more than one scene (scene.add), a parameter that is not assigned yet is scene-assigned by its first set, so it changes in the active scene only and the others keep the value they had; this is one undo step. With one scene, and for normalize, a parameter on the expression pedal or one scene.assignParam refuses, the setting has one value shared by all scenes. |
| [`block.setSlimSize`](#blocksetslimsize) | yes | edit | Sets a NAM block's model size, trading CPU for accuracy: 0 is the lightest, 1 the full model (only slimmable captures, which NAM A2 files are, change; a capture of one size keeps it; IR blocks refuse). |
| [`capture.addToRig`](#captureaddtorig) | yes | local | Adds the trained capture to the rig being played as a NAM block (chain.addToneFromFiles), so it can be compared with the real device. Returns what that command returns. |
| [`capture.cancelRecording`](#capturecancelrecording) | yes | local | Stops a recording that is going and throws away what was recorded; also discards a finished recording so that it can be made again (in the record or check step). |
| [`capture.cancelTraining`](#capturecanceltraining) | yes | local | Stops the trainer (the child process is killed) and returns the training status. |
| [`capture.checkTrainer`](#capturechecktrainer) | no | local | Finds out whether the trainer can be run: starts Python and imports neural-amp-modeler, which takes a few seconds. Answers pythonFound, packageFound, versionOk (version 0.13.0 or newer, the first whose standard training makes NAM A2 files), the version and a message saying what to do when it is not usable. |
| [`capture.create`](#capturecreate) | yes | local | Starts a capture session and returns it, at the source step. Its folder is <library>/Captures/<id>; the session survives a restart. |
| [`capture.delete`](#capturedelete) | yes | local | Deletes a capture session's folder (the recordings and an unsaved model); a capture already saved to the library is a file of its own and stays. Stops a run that belongs to it. |
| [`capture.get`](#captureget) | no | read | Returns one capture session: its settings, the step it is at (and what stops it from going on, blocks), levels, recording, checks, training and files. During training it carries the trainer's progress too. |
| [`capture.getSettings`](#capturegetsettings) | no | local | Returns the capture settings: the Python executable the trainer runs under (empty: python3 on the path), the trainer's standard input file and whether it is the NAM project's version 3 file (by checksum), and the level target. The standard input file is published by the NAM project and is not shipped with Pedaleira. |
| [`capture.levelStatus`](#capturelevelstatus) | no | local | Returns the levels: the return's current peak, peak hold, RMS and clipped samples (dry/wet: both inputs), the target band and whether the hold is inside it (state silent, low, high, clipping or inRange), and the noise floor when measured. Each call also records the readings in the session, which is what lets the wizard leave the levels step. |
| [`capture.list`](#capturelist) | no | read | Lists the capture sessions, newest first: id, name, source, method, step, training state, ESR and the saved file when there is one. |
| [`capture.measureNoiseFloor`](#capturemeasurenoisefloor) | yes | local | Mutes the tone for a moment and measures the return's noise (RMS) over the given time. The result shows in capture.levelStatus as noiseFloor. |
| [`capture.recordingStatus`](#capturerecordingstatus) | no | local | Returns the recording's state (none, recording, writing, complete, failed), progress from 0 to 1, seconds recorded and left, the return's peak so far, clipped samples and, for a re-amp, the latency measured once enough has come in. |
| [`capture.review`](#capturereview) | no | local | Returns what the review step shows: the trained file, the ESR and its band (great under 0.01, not bad under 0.035, might be okay under 0.1, poor under 0.3, else failed), the calibration and the metadata the saved file will carry. |
| [`capture.runChecks`](#capturerunchecks) | yes | local | Runs the data checks on the finished recording and returns the session with the report (checks): length, peak level against the target band, clipping, noise floor, latency, DC offset and, for dry/wet, that both inputs have signal and the wet is not a copy of the dry. Each check is pass, warn or fail; the verdict is the worst. Moves a capture at the record step on to the check step. |
| [`capture.save`](#capturesave) | yes | local | Saves the capture into the library: the trained file moves to <library>/Captures/<name>.nam (a copy with the name, gear, gain tag and calibration set in its metadata when the trainer did not write them). Returns the file. |
| [`capture.setChannels`](#capturesetchannels) | yes | local | Sets which device channels the capture uses, counted from 0: for a re-amp the send output and the return input, for dry/wet the dry and wet inputs. Also the calibration: how many dBu 0 dBFS at the return input is (saved in the capture as output_level_dbu; the Volt 876 is +12.5) and, when known, at the send output (input_level_dbu). The processor has the main stereo pair only for now, so 0 and 1 are the usable channels; the answer warns about a channel the device does not have. |
| [`capture.setLevels`](#capturesetlevels) | yes | local | Sets the send level of the test tone and of the signal that is recorded (its peak, in dBFS), the level target and band the return has to reach, and for dry/wet how long to record. The target defaults to the capture settings' level target, the band to 3 dB either side. Changing the send level or the target clears the peak readings. |
| [`capture.setSettings`](#capturesetsettings) | yes | local | Sets any of the capture settings and returns them all (capture.getSettings). |
| [`capture.setSource`](#capturesetsource) | yes | local | Changes what the capture is: its name, source, method, gain tag, model size, gear and notes. The source and the method can only change up to the wiring step. Returns the session. |
| [`capture.setStep`](#capturesetstep) | yes | local | Moves the wizard to a step: one forward, when what the current step needs holds (the answer of a refused move says what is missing), or any step back, which throws away what the steps left behind had made (going back to record discards the recording). Steps: source, wiring, levels, record, check, train, review. |
| [`capture.startRecording`](#capturestartrecording) | yes | local | Starts the recording in the record step. Re-amp: the trainer's standard input file (capture.setSettings) plays on the send channel at the send level and the return is recorded, sample for sample with what is played; it takes about three minutes. Dry/wet: both inputs are recorded together while you play, for the recording time. The audio device must run at 48 kHz. Poll capture.recordingStatus; the files are written when it ends. |
| [`capture.startTone`](#capturestarttone) | yes | local | Starts the levels run in the levels step: the rig is muted, a 1 kHz test tone plays on the send channel at the send level and the return is metered (for dry/wet there is no tone: the two inputs are metered while the guitar is played). Poll capture.levelStatus. Standalone application only. |
| [`capture.startTraining`](#capturestarttraining) | yes | local | Starts the trainer (neural-amp-modeler 0.13.0 or newer, whose standard training makes NAM A2 files) as a child process and returns at once. From the check step, the checks must not have failed unless overrideChecks is set. Poll capture.trainingStatus. Training an already trained capture again (from review) starts over. |
| [`capture.stopTone`](#capturestoptone) | yes | local | Ends the levels run (the readings are kept in the session) and unmutes the rig. |
| [`capture.trainingStatus`](#capturetrainingstatus) | no | local | Returns the training state (none, checking, running, done, failed, cancelled), the epoch and progress, the time left, the validation ESR with its band (great, notBad, mightBeOkay, poor, failed), the latency the trainer used, whether its own data checks passed, the last lines of its output, and an error message when it failed. A finished training whose file is not NAM A2 is a failure. Done moves the capture to review. |
| [`chain.addBlock`](#chainaddblock) | yes | edit | Adds a block of a registered kind (see kinds.list), such as utility.gain, at its default settings; unlike a tone it has no model to load and is live when the command returns. |
| [`chain.addPlugin`](#chainaddplugin) | yes | edit | Adds a block that hosts one of the scanned effect plugins and returns it; the plugin is created on the message thread after the command returns, so the block shows as loading until state.waitUntilLoaded (or a poll of state.get) sees it loaded, and its parameters (block.listParams) exist from then on. A plugin that will not load leaves a bypassed placeholder under its name. |
| [`chain.addToneFromFiles`](#chainaddtonefromfiles) | yes | edit | Adds ONE tone block from local files and starts loading it in the background: a single .nam or .wav (IR) file, a folder of them, or several files of the same kind that become the selectable models of that one block (call once per block to add an amp and a cab). |
| [`chain.clearBranch`](#chainclearbranch) | yes | edit | Removes the branch so both lanes take their own input again. |
| [`chain.duplicateBlock`](#chainduplicateblock) | yes | edit | Clones a tone block with its model and every setting, and returns the new block's id. |
| [`chain.moveBlock`](#chainmoveblock) | yes | edit | Moves a tone block across to the other lane at the given position (stereo mode only; use chain.reorder to move within a lane). |
| [`chain.redo`](#chainredo) | yes | edit | Redoes the chain edit most recently undone. |
| [`chain.removeBlock`](#chainremoveblock) | yes | edit | Removes a tone block from the chain. |
| [`chain.reorder`](#chainreorder) | yes | edit | Reorders the blocks of one lane; signal flows through them in the order given. |
| [`chain.reset`](#chainreset) | yes | edit | Returns to the factory default: an empty mono chain, preset-managed parameters at their defaults and no active preset, as one undoable step. |
| [`chain.setBranch`](#chainsetbranch) | yes | edit | Feeds one lane from the other lane's signal after a chosen tone block instead of from its own input (stereo mode only); calling it again moves the tap. |
| [`chain.setInputMode`](#chainsetinputmode) | yes | edit | Chooses how a stereo source feeds the chain: stereo (the natural routing), left or right (one channel onto both), or dual (a mono chain run once per channel). |
| [`chain.setStereo`](#chainsetstereo) | yes | edit | Switches between one mono chain and two independent left / right lanes. |
| [`chain.swapLanes`](#chainswaplanes) | yes | edit | Swaps the left and right lanes wholesale (stereo mode only). |
| [`chain.undo`](#chainundo) | yes | edit | Undoes the last chain edit (structure, tones, block settings, EQ, stereo mode, preset load); global parameters are not part of the history. |
| [`commands.describe`](#commandsdescribe) | no | read | Lists every command with its summary, parameters and whether it mutates state, or describes the one named. |
| [`events.subscribe`](#eventssubscribe) | no | read | Turns change notifications on or off for the calling connection. With revision on, the server sends {"event":"revision","rig":...,"revision":N} whenever the revision of the rig the connection is working on (the live rig or its selected sandbox) changes, so a client can refetch state.get without polling. |
| [`expression.assignParam`](#expressionassignparam) | yes | edit | Sets a block parameter aside for the expression pedal. The pedal itself is not there yet; what the flag does today is keep the parameter out of the scenes: it is unassigned from them (the active scene's value stays as the shared one) and scene.assignParam refuses it, so a scene switch never pulls a pedal-driven value away. |
| [`expression.unassignParam`](#expressionunassignparam) | yes | edit | Clears a parameter's expression pedal flag, so it can be scene-assigned again. |
| [`favourite.set`](#favouriteset) | yes | edit | Marks a preset as a favourite or takes the mark away. Favourites are a set of preset ids kept in library.json; a factory preset can be one. |
| [`footswitch.getState`](#footswitchgetstate) | no | read | Returns what a pedalboard view draws: mode (preset, scene, stomp or hybrid), hybridRows (what the top row A to D and the bottom row E to H do in hybrid mode), bank and bankCount (preset banks), scenePage, activeScene, sceneCount (scenes in use), bpm, and the eleven switches (A to H, bankUp, bankDown, tempo), each with its role in the current mode (preset, scene, stomp, bankUp, bankDown, presetNext, presetPrevious or tapTempo), label, colour (a hint: the category of a stomp's first block, else the role), lit, held, empty (nothing behind the switch, also a scene the preset does not use) and its target (scene, presetId and presetIndex, or blockIds and momentary). |
| [`footswitch.press`](#footswitchpress) | yes | edit | Presses a footswitch. A to H act by mode: Preset loads preset bank x 8 + switch of the preset list, Scene selects scene A to H, Stomp flips the switch's assigned blocks, Hybrid does per row what footswitch.setHybridRows says. bankUp and bankDown move the preset bank (and, in a hybrid with one scene row, flip that row between scenes A to D and E to H); where there is nothing to page (Scene and Stomp mode) they step to the next or previous preset. tempo taps the tempo (see tempo.tap). Follow it with footswitch.release. |
| [`footswitch.release`](#footswitchrelease) | yes | edit | Releases a footswitch; a momentary stomp flips its blocks back, nothing else acts on the release. |
| [`footswitch.setHybridRows`](#footswitchsethybridrows) | yes | settings | Sets what each row does in hybrid mode: scene, stomp or preset. A stomp row uses its own switches' assignments. With both rows on scenes the top row is scenes A to D and the bottom E to H; a single scene row shows A to D and bankUp / bankDown flip it to E to H. A preset row shows four presets, two show eight, and the bank switches move by that many. Machine-wide, like the mode. |
| [`footswitch.setMode`](#footswitchsetmode) | yes | settings | Sets what the eight footswitches do: preset, scene, stomp or hybrid. A machine-wide setting, kept across rigs, presets and restarts. |
| [`grid.convert`](#gridconvert) | yes | edit | Converts the rig between the legacy routing (one chain, or two lanes with an optional branch) and the grid, as one undo step. To grid: lane position i becomes slot i of row 0 or 1, and lossy lists what does not carry over. A branch becomes a split of row 0 into row 1 that does not rejoin (A/B, both at 0 dB), with the trunk as row 0. To legacy: only a grid with the shape of a legacy rig converts (one stereo or dual-mono row, two mono rows, or two mono rows joined by such a split, on inputs 1 and 2 and output pair 1); otherwise it fails with the reasons. |
| [`grid.get`](#gridget) | no | read | Returns the rig's routing: mode (legacy or grid), legacyProjectable (whether a grid could be written as a legacy rig), and in grid mode rows (each with index, format, input, output, slots as the eight block ids of the row with empty slots included, active per slot (a block that is run: in a routed row and, for the lower row of a split, inside the split's range), activeSlots as [first, end), routed, source (device, split, rows or none), sink (device, row, mix or none), runsAs (the format the row runs in), latencySamples and padSamples at the chain rate, cpu, inputMissing and outputFallback), splits (each with id, row, lowerRow, splitAt, mixAt or null, type, active, lowerActive, params by id, alignment as the samples paths a and b wait at the mixer, and meters), feeds (each from, to, active and padSamples), groups (the rows that are processed together: tied by a split or a feed), latencySamples (as reported to the host), chainLatencySamples, cpu (total and critical, the plan's estimate as fractions of one core), imageStage (none in grid mode), legacyControls (per legacy image control, rows when rows 0 and 1 carry it and held when it rests at neutral and does nothing) and io (the device's input channels and output pairs, each with whether it is available; a channel or pair a row asks for that the device lacks is listed as unavailable). |
| [`grid.moveBlock`](#gridmoveblock) | yes | edit | Moves a block to a slot of the grid. swap exchanges it with whatever is there (a block or an empty slot); insert puts it there and pushes the blocks from that slot on toward the row's nearest empty slot, and fails when the row is full. A NAM block that lands in a row of another format (mono or stereo against dual mono) is rebuilt for it. |
| [`guitar.analyze`](#guitaranalyze) | yes | edit | Measures every take of a guitar profile again and rebuilds its summary (after clips were replaced on disk, or when the measurements changed with a new version); returns the profile, with unreadable listing take files that could not be read. |
| [`guitar.create`](#guitarcreate) | yes | edit | Creates a guitar profile and returns it; the first one becomes the default. Clips are added afterwards with guitar.importClip or guitar.record.start. |
| [`guitar.delete`](#guitardelete) | yes | delete | Deletes a guitar profile with its clips (to the system trash where there is one) and returns the remaining list. |
| [`guitar.get`](#guitarget) | no | read | Returns one guitar profile in full: name, pickupType, positions, tuning, notes, takes (each a recorded DI clip of one position, soft, hard, chords or notes, with its measurements: peakDb, rmsDb, lufs, crestDb, centroidHz, the band balance, lowEndShare, hum at 50 and 60 Hz and the noise floor) and the summary. Tones are built for a guitar by rendering its clips (render.clip). |
| [`guitar.importClip`](#guitarimportclip) | yes | edit | Attaches an existing WAV file to a guitar profile as one take of one position (copied into the profile, replacing a take already there), measures it and returns the profile. The clip should be the dry DI signal of the guitar, mono, a few seconds long. |
| [`guitar.list`](#guitarlist) | no | read | Lists the guitar profiles: id, name, pickup type, positions, tuning, how many takes each has, its summary (outputLevel low/medium/high, brightness dark/balanced/bright, noise quiet/moderate/noisy, with the numbers behind them) and which one is the default. |
| [`guitar.record.cancel`](#guitarrecordcancel) | yes | edit | Stops a take recording that is in progress and discards what was recorded. |
| [`guitar.record.start`](#guitarrecordstart) | yes | live | Starts recording a take from the live audio input: the raw signal of one input channel, before the rig, for the given number of seconds, starting now. Poll guitar.record.status; when the time is up the take is saved into the profile and measured. Replaces a recording in progress. It listens to what the user is playing, so for a command server client it counts as changing the live rig (approval). |
| [`guitar.record.status`](#guitarrecordstatus) | no | read | Returns the take recording's state: idle, recording (with recordedSeconds, progress from 0 to 1 and peakDb, the input's peak so far), writing (the buffer is full and the take is being saved and measured), done (with result, the new take and its measurements) or failed (with error). |
| [`guitar.setDefault`](#guitarsetdefault) | yes | edit | Makes a guitar the default one: the one render.clip uses when none is named. |
| [`guitar.update`](#guitarupdate) | yes | edit | Changes a guitar profile's name, pickup type, positions, tuning or notes; fields left out stay. A position that has takes cannot be removed. |
| [`kinds.list`](#kindslist) | no | read | Lists every block kind this build knows: id, name, category, the family it is filed under within its category and a line about it (family and blurb: every shipped amp is a kind of its own, filed under High gain, Clean or Crunch with its character line; empty for the other kinds), whether chain.addBlock can create it (nam and ir blocks come from chain.addToneFromFiles instead), the mix a new block of it starts at (defaultMix), how its mix knob blends dry and wet (mixLaw: crossfade is dry x (1 - mix) + wet x mix; dryPreserving keeps the dry signal at unity up to mix 0.5, where the wet signal reaches unity too, and fades it above that), whether its tail rings out after a bypass (spillover: the kind then has a spillover switch among its parameters, on by default) its own parameters with their ranges, units and defaults, and its readings (read-only values it reports while it runs, such as a compressor's gain reduction; meters.get returns them). categories names every category a kind can be filed under. |
| [`library.get`](#libraryget) | no | read | Returns the library: root (its folder), setlists (as setlist.list), current (setlist, slot or 0, and presetId, the active preset's id), favourites (preset ids), recents (as recent.list), unsaved (the rig differs from the active preset; with no active preset, from the empty default) and legacyPresets (how many presets of the old .t3kpreset store library.importLegacyPresets would bring over). |
| [`library.importLegacyPresets`](#libraryimportlegacypresets) | yes | edit | Copies the user presets of the old .t3kpreset store into a setlist as JSON presets, keeping their ids. The setlist is made when missing and a second one is started when it is full. The old files are neither moved nor deleted, and presets imported before are skipped. (An empty library brings the old preset list over by itself the first time it is used; this is for presets deleted since, or a library that was not empty.) Returns { imported, skipped, failed, setlists }. |
| [`library.rescan`](#libraryrescan) | yes | edit | Reads the library's folders again from scratch, for changes made outside the app (a file browser, git) that the usual time and size check could miss. With tidy the preset files are also renamed to what their slots and names make them (a file added without a slot number gets one). Returns library.get plus renamed. |
| [`looper.clear`](#looperclear) | yes | edit | Erases the loop and all its layers and goes back to empty. |
| [`looper.get`](#looperget) | no | read | Returns the looper's state as the audio thread last published it: state (empty, arming, recording, playing, overdubbing or stopped; arming is a record action waiting for the loop's memory, which is reserved on the first record and freed after the loop has been empty for a while), memoryReady, memoryBytes and memoryFailed (the reservation was refused), closing (the first recording is running on to the next beat, with quantise on), queued (actions waiting for the next audio block), channels (1 or 2), lengthFrames and lengthSeconds (of the loop as recorded), playbackLengthSeconds (twice that at half speed), recordedSeconds (while recording), positionFrames, positionSeconds and position (0 to 1) in the loop, layers (the recording and the overdubs in the sound now; the one being recorded counts), canUndo, redoLayers, canRedo, layersFull, maxLayers, maxSeconds, capacitySeconds, and options: halfSpeed, reverse, oneShot, quantise and levelDb. The loop is not saved with a preset or the plugin state. |
| [`looper.overdub`](#looperoverdub) | yes | edit | Layers new playing on top of the loop (up to four layers after the first recording). Recording: ends the first recording and starts overdubbing from the top of the loop. Playing or stopped: starts overdubbing. Overdubbing: ends the layer and keeps playing. Nothing is recorded in half speed or reverse: overdubbing runs at normal speed, forwards. Ignored with no loop, and when all layers are used (layersFull). |
| [`looper.play`](#looperplay) | yes | edit | Plays the loop. Stopped: from the start (or, in reverse, from the end). Recording: ends the first recording and plays it. Overdubbing: ends the layer and keeps playing. |
| [`looper.record`](#looperrecord) | yes | edit | Records the loop from the main output (after the tone stack, before the output level). Empty: starts the first recording, which sets the loop's length when it ends. Recording: ends it and starts playing (with quantise on, at the nearest whole number of beats). With a loop already there: throws it away and starts a new recording. Up to 60 seconds; at the limit the loop closes by itself. The first record after startup (or after the memory was freed) reserves the loop's memory off the audio thread: the state reads queued, then arming, and recording starts in the first block after the memory is there (a few milliseconds). Takes effect at the start of the next audio block. Answers with looper.get's state, which does not show the action yet. |
| [`looper.redo`](#looperredo) | yes | edit | Puts back the overdub layer looper.undo took out, if no new overdub has started since. |
| [`looper.setOptions`](#loopersetoptions) | yes | edit | Sets how the loop plays; give at least one. halfSpeed plays at half speed, an octave down and twice as long (playbackLengthSeconds); reverse plays backwards; oneShot plays the loop once and stops instead of repeating; quantise rounds the length of the first recording to a whole number of beats at the tempo of the moment it ends; levelDb is the loop's playback level (the rig's own sound is not touched). They take effect within a block and can change while playing. Answers with looper.get's state. |
| [`looper.stop`](#looperstop) | yes | edit | Stops the loop and puts it back at the start, keeping what was recorded. Recording: ends the first recording and stops. Overdubbing: ends the layer first. |
| [`looper.undo`](#looperundo) | yes | edit | Takes the last overdub layer out of the sound (the first recording stays; use looper.clear for that). While overdubbing it throws away the layer being recorded and keeps playing. The layer is kept for looper.redo until a new overdub starts. |
| [`meters.get`](#metersget) | no | read | Returns the current levels in dB (floor -60): input and output as [left, right], each block's in and out by block id (with readings, by reading id, for a kind that has them: see kinds.list), the audio thread's CPU load (0 to 1) and the output's stereo correlation (-1 to 1). Also rows (in grid mode, per row what entered it and what it sent, as [left, right]), splits (in grid mode, per split what the splitter sent down paths A and B as split and what reached the mixer from each as mix), outputs (per output pair in use, its level after the output strip) and planCpu (total, critical and rows: what the rig's blocks are estimated to cost, as fractions of one core; critical is the most one thread has to carry with multi-core on). |
| [`midi.getMap`](#midigetmap) | no | read | Returns the MIDI map: the channel filter (0 is omni), each mapping's target, source (cc or note) and number, and defaults, the fixed controls (scene select, tap tempo, tuner, footswitch mode and the fourteen looper controls, CC 48 to 61) each with its action, cc and whether a mapping of the user's own overrides it. |
| [`midi.remove`](#midiremove) | yes | edit | Removes the mapping of one target. |
| [`midi.setCc`](#midisetcc) | yes | edit | Maps a MIDI CC number to a target, replacing any mapping that target already had. |
| [`midi.setChannelFilter`](#midisetchannelfilter) | yes | edit | Limits MIDI control (mappings and program changes) to one channel, or to all of them. |
| [`nam.clearRecent`](#namclearrecent) | yes | local | Empties the list nam.recent returns. The files stay findable by name with assets.list. |
| [`nam.downloaded`](#namdownloaded) | no | read | Finds the file a capture of a TONE3000 tone was downloaded to before, in the library's TONE3000 folder: {path} when it is there, {path: ""} when it is not (never downloaded, or the file was deleted). The TONE3000 page asks before it downloads, so a capture is fetched once. |
| [`nam.inspect`](#naminspect) | no | read | Tells whether Pedaleira can load a NAM capture, without loading it: its architecture (architecture as the file says it, architectureLabel as a player knows it, such as WaveNet A2, WaveNet A1, LSTM, ConvNet, Linear or Slimmable A2), a2 (true for NAM A2), sampleRate (the rate it was trained at, when the file says; it always runs at that rate, resampled when the session runs at another, and at 48 kHz when the file does not say), supported (every architecture NAM builds loads; false for a file that is not a capture, a NAM version or architecture Pedaleira cannot read, or damaged settings) and, when it is not supported, unsupportedReason in a sentence that can be shown as it is. Weights that do not fit the settings are only found when the file is loaded. modelName is the name in the file's metadata, empty when it has none. |
| [`nam.keepDownload`](#namkeepdownload) | yes | local | Keeps a capture or IR just downloaded from TONE3000 in the library: moves `file` into <library>/TONE3000 under a name made from the tone's title and the capture's name ("Title - Capture.nam", unsafe characters replaced, a number added when the name is taken) and remembers which tone and capture it is and who made it, so it persists across restarts, is found by assets.list (origin tone3000, with its tone3000 credit) and is reused by nam.downloaded. Returns {path}, the kept file. The file is then loaded with chain.addToneFromFiles. |
| [`nam.recent`](#namrecent) | no | read | Lists the captures (or IRs) the player loaded from a file, newest first: name (the file name without its extension), path (the file to load again: the original when it is still there, else the copy the model stash kept), fromStash (true for that copy), loadedAt (milliseconds since 1970), for a capture what nam.inspect says, and tone3000 (the tone, creator and licence on TONE3000) for a file that came from there. A file that is gone from both places is left out. The stash drops copies that were not used for a week, so an old entry whose original moved away can disappear. |
| [`param.get`](#paramget) | no | read | Returns one global parameter in the same shape as param.list. |
| [`param.list`](#paramlist) | no | read | Returns every global (faceplate and settings) parameter by id with its value in real units, normalized value, range, default, name, type and unit. |
| [`param.set`](#paramset) | yes | edit | Sets one global parameter to a value in real units (dB, Hz, a choice index, 0 or 1 for a switch), clamped to the parameter's range, and returns the parameter as it now is. |
| [`permissions.get`](#permissionsget) | no | read | Returns what command server clients may do, one switch per permission class: read (library, presets and rig state), edit (create and edit presets and rigs), render (render clips to measure a tone), live (change the rig being played; off by default, and then such a request waits for approval), delete (delete presets, setlists and guitars; off by default) and settings (audio, device and machine-wide settings; off by default). |
| [`permissions.set`](#permissionsset) | yes | local | Changes the permission switches for command server clients; switches left out stay as they are. Kept across restarts. |
| [`plugins.addFolder`](#pluginsaddfolder) | yes | settings | Adds a folder to search for plugins on top of the formats' default locations (kept across sessions) and returns the added folders; the next plugins.scan looks there. |
| [`plugins.cancelScan`](#pluginscancelscan) | yes | settings | Stops a running plugin scan after the plugin it is on and returns the scan status; what was scanned so far is kept. |
| [`plugins.list`](#pluginslist) | no | read | Lists every plugin the scanner knows with its name, vendor, format, category, version, channels, file and the identifier chain.addPlugin takes, and a status: ok (an effect that can be added), failed scan, not an effect (instruments and MIDI-only plugins) or missing (no longer found); also the added folders and the scan status. |
| [`plugins.removeFolder`](#pluginsremovefolder) | yes | settings | Removes a folder added with plugins.addFolder and returns the added folders; plugins that were only found there show as missing after the next plugins.scan. |
| [`plugins.scan`](#pluginsscan) | yes | settings | Starts a scan for VST3 and Audio Unit plugins in the background, replacing one that is running, and returns the scan status; each plugin is scanned in a helper process, so one that crashes or hangs is blacklisted with the reason instead of taking the app down, and files that have not changed since the last scan are not scanned again. Answers unavailable when the helper executable is not installed. |
| [`plugins.scanStatus`](#pluginsscanstatus) | no | read | Returns the plugin scan's progress: state (idle, scanning, finished, cancelled or unavailable), the file being scanned, done and total candidates, and how many needed scanning and failed. |
| [`preset.copyToSetlist`](#presetcopytosetlist) | yes | edit | Copies a preset into another setlist under a new id, into the given empty slot or the first empty one. The source may be a factory preset; model files are shared, not copied. |
| [`preset.delete`](#presetdelete) | yes | delete | Moves a preset's file to the system trash and leaves its slot empty (factory presets cannot be deleted); the loaded chain is left as it is. |
| [`preset.deleteSlot`](#presetdeleteslot) | yes | delete | Moves the preset file of a slot to the system trash and leaves the slot empty; the other presets keep their slots and the loaded chain is left as it is. |
| [`preset.duplicateSlot`](#presetduplicateslot) | yes | edit | Copies a preset within its setlist under a new id, as "<name> copy", into the given empty slot or the first empty slot after it. |
| [`preset.exportJson`](#presetexportjson) | yes | edit | Writes a preset as a JSON file at any path: the preset with the given id, or without an id the rig as it is now. Model files and large plugin states go into an Assets folder beside the file (the layout preset.importJson reads), or into the file itself as base64 with embedAssets. |
| [`preset.importForeign`](#presetimportforeign) | yes | edit | Imports a preset read from another product (a foreign preset JSON, docs/import.md; tools/import/quadcortex_dump.py writes them from a Quad Cortex) into an empty slot of a setlist, as a grid preset: its blocks become ours through a mapping table (the built-in one for the file's source, or the one at `mapping`), rows and splits keep their layout as far as the grid allows, and scenes carry what the scene model can hold. Amps, cabs and captures are never mapped: their slots stay empty for the player to fill. Returns the new preset as preset.importJson does, plus report: { mapped, approximated, unmapped, droppedParams, notes }, each entry naming the block by row, position and the source's model. Loads nothing. |
| [`preset.importJson`](#presetimportjson) | yes | edit | Reads a JSON preset (or a .t3kpreset) from any path into an empty slot of a setlist: the given one or the first empty one. Its assets are taken from the file, from an Assets folder beside it or from the library, and stored in the library; ones nobody has are listed in missingAssets and the preset is imported all the same. Loads nothing. |
| [`preset.list`](#presetlist) | no | read | Lists the presets of the current setlist in slot order as { presets: [{ id, name, factory, slot }], setlist: { id, name, readOnly } }. Empty slots are left out; a preset's slot is also its MIDI program number plus one and its place on the footswitches' preset banks. |
| [`preset.load`](#presetload) | yes | edit | Loads a preset, replacing the chain and the preset-managed parameters, and starts loading its models in the background (one undoable step for the chain). The id may be of any setlist's preset (a favourite, a recent); the current setlist only changes through setlist.select and preset.loadSlot. A block whose model file is missing from the library comes up failed under its name and the rest of the rig loads. |
| [`preset.loadSlot`](#presetloadslot) | yes | edit | Loads the preset in a slot (as preset.load does) and makes its setlist the current one. |
| [`preset.move`](#presetmove) | yes | edit | Moves a preset up or down among the presets of the current setlist: the slots in use stay the same and the presets trade places in them, which also sets the order MIDI program changes follow. preset.moveSlot moves one to a slot by number. |
| [`preset.moveSlot`](#presetmoveslot) | yes | edit | Moves a preset to another slot of its setlist and renumbers the files. insert (the default) shifts the presets and gaps in between one slot towards where it came from; swap exchanges the two slots. |
| [`preset.previewForeign`](#presetpreviewforeign) | no | read | The dry run of preset.importForeign: reads and converts a foreign preset the same way, with the same mapping table, and returns what the import would make without saving or loading anything: { name, report }, the report as preset.importForeign returns it. Every block of the file appears in exactly one of the report's mapped, approximated (type block) or unmapped lists, so a caller can show what each block becomes before importing. |
| [`preset.rename`](#presetrename) | yes | edit | Renames a preset of the user's and its file; its id and slot stay (factory presets cannot be renamed). |
| [`preset.save`](#presetsave) | yes | edit | Saves the current chain and preset-managed parameters as a preset of the current setlist, makes it the active preset and returns its id, setlist and slot. Saving under the name of a preset of the setlist replaces that preset; otherwise it goes into the first empty slot. With the read-only factory setlist current it goes to the first setlist of the user's own ("My Presets" is made when there is none), which becomes current. |
| [`preset.saveToSlot`](#presetsavetoslot) | yes | edit | Saves the current chain and preset-managed parameters into a slot of a setlist, makes that the active preset and the setlist the current one. A preset already in the slot is replaced and keeps its id; name may then be omitted to keep its name too. |
| [`recent.list`](#recentlist) | no | read | Lists the presets loaded last, most recent first (at most 32), as { recents: [{ id, name, setlist, slot, factory }] }. Presets that no longer exist are left out. |
| [`render.clip`](#renderclip) | no | render | Plays recorded guitar clips through a rig offline and returns measurements of what came out, one entry in renders per clip (and per scene): input (the clip's own peak, RMS and LUFS), levels (peakDb, truePeakDb, rmsDb, crestDb, lufs), gainDb (output RMS minus input RMS), spectrum (the share of energy in sub, bass, lowMid, mid, highMid, presence and air, in dB and as fractions, the spectral centroid and the low-end share), stereo (correlation, width, balanceDb), distortion (percent: how much of the output a linear filter of the input cannot explain; time effects longer than the fit and modulation count too, so bypass them to measure the amp alone), tail (seconds the output keeps sounding after the clip ends) and latency. dynamics pairs each position's soft and hard takes: their distortion, cleanupRatio (soft over hard) and compressionDb (how much of the playing dynamics the rig removes). The rig is the connection's selected sandbox, the one named, or a private copy of the live rig; the rig being played is never rendered through. The same call on the same rig returns the same numbers. A sandbox cannot be edited while it renders, and at most two renders run at a time in the whole application: a third answers unavailable. |
| [`render.compare`](#rendercompare) | no | render | Measures a reference recording and the rig's render of the chosen clips (all of them joined) the same way and reports how far apart they are: reference and rig (levels, spectrum, stereo), difference (rig minus reference: lufs, crestDb, bandsDb per band, centroidOctaves, correlation, width) and distance, a score per group (loudness, dynamics, tone, stereo) and overall, where 0 is identical and 1 is a difference a listener would not miss (3 dB of loudness or crest factor, 3 dB RMS across the bands or a third of an octave of centroid, 0.3 of correlation). Takes the clip arguments of render.clip. |
| [`render.probe`](#renderprobe) | no | render | Measures a rig with a test signal instead of a guitar clip. response: plays noise at levelDb and returns the rig's gain per frequency (points: hz, gainDb, and coherence, which falls below 1 where the rig is not behaving linearly at that level) plus the overall distortion percent; meaningful for linear-ish rigs or low levels. levelCurve: plays a sine at frequencyHz in steps from -60 to 0 dBFS and returns the output level for each (points: inputDb, outputDb, gainDb, all as sine peak levels), which shows compression and gain staging. noise: plays silence and returns the noise floor (noiseFloorDb RMS, peakDb) and any mains hum. Uses the rig render.clip would. |
| [`roots.add`](#rootsadd) | yes | local | Adds a folder to the ones clients may read files from (captures, IRs, reference recordings). Kept across restarts. |
| [`roots.list`](#rootslist) | no | read | Lists the folders a command server client may name in a path argument (roots): the library, the local model stash, the scratch folder and the ones the user added (added). A path anywhere else is refused. |
| [`roots.remove`](#rootsremove) | yes | local | Removes a folder added with roots.add. |
| [`row.setFormat`](#rowsetformat) | yes | edit | Sets what a row's buffer carries: mono (one channel, panned onto its output), stereo (a stereo pair) or dual (two independent mono signals through the same blocks, each amp running once per channel). Amps in the row rebuild when the change moves them between one and two voices. |
| [`row.setInput`](#rowsetinput) | yes | edit | Sets where a row's signal comes from: none (the row is not fed and does not run; its blocks are kept) or a device channel, or the pair starting at it. A pair can be summed to mono. An input the device does not have is silence and is reported as inputMissing; the setting is kept for a device that has it. |
| [`row.setInputStage`](#rowsetinputstage) | yes | edit | Sets a row's share of the input stage; fields left out stay as they are. trimDb is the row's own level on top of the rig's input level (applied as the signal enters the row, after the gate). gate switches the noise gate for the row's input (the gate's settings are the rig's). pitch asks for the pitch shifter: one input runs it, the first that asks, and the others are delayed to stay in step. |
| [`row.setMix`](#rowsetmix) | yes | edit | Sets a row's send to its output: volume, pan, mute, solo and polarity; fields left out stay as they are. Pan is constant power on a mono row (hard left or right is unity) and a balance on a stereo or dual-mono row (unity at the centre, the far side falling to silence). Solo silences the outputs of the rows that are neither soloed nor fed by a soloed row. A row that feeds another row goes in at its volume, mute and polarity; its pan and solo wait for a device output. Changes glide, without a gap in the audio. |
| [`row.setOutput`](#rowsetoutput) | yes | edit | Sets where a row's signal goes: none (the row does not run), a pair of device outputs, or another row (target row with toRow), whose input it is then summed into at the row's volume, with the rows that feed one row kept in step. A fed row has no input of its own and is not the lower row of a split, and feeds form no loop; otherwise the command fails with the reason and nothing changes. A pair the device does not have falls back to pair 1 and is reported as outputFallback; the setting is kept. |
| [`sandbox.applyToLive`](#sandboxapplytolive) | yes | live | Copies a sandbox rig onto the rig being played, the way loading a preset would: the chain with its scenes and the preset-managed parameters, as one undo step (chain.undo on the live rig brings the previous chain back). The sandbox is kept. This is the step that changes what the user hears: with the live permission off it answers needs_approval and runs when the user approves; what is applied then is the sandbox's rig as it was when this was called, so edit first and ask after (asking again after an edit replaces the earlier request). Default: the calling connection's selected sandbox. |
| [`sandbox.create`](#sandboxcreate) | yes | edit | Creates a sandbox rig: a second rig that is never heard, for building and measuring a tone without disturbing the one being played. It starts as a copy of the live rig (from live), empty, or from a preset, and is selected for the calling connection unless use is false: from then on every rig command of that connection (chain.*, block.*, scene.*, param.*, preset.*, state.* and the rest) acts on the sandbox instead of the live rig. A copy loads its blocks in the background like a project being opened, so call state.waitUntilLoaded before editing them. A sandbox belongs to the connection that made it: no other client can select, edit, render, apply or discard it. At most four exist at a time; a connection's sandboxes are discarded when it closes. |
| [`sandbox.discard`](#sandboxdiscard) | yes | edit | Discards one of the calling connection's sandbox rigs and frees what it held; the connection goes back to the live rig if it had it selected. Default: the calling connection's selected sandbox. |
| [`sandbox.list`](#sandboxlist) | no | read | Lists the calling connection's own sandbox rigs (a local caller sees every client's): id, name, the client that made it, whether a render is running through it and whether it is the calling connection's selected rig; selected names that rig (live when none). |
| [`sandbox.use`](#sandboxuse) | yes | edit | Selects the rig the calling connection's rig commands act on: one of its own sandboxes by id, or live for the rig being played. |
| [`scene.add`](#sceneadd) | yes | edit | Adds the next scene (a preset starts with one and can have up to eight). The new scene starts as a copy of the active scene's block states and assigned values, with its default name; the active scene stays the same. Returns the scenes (as scene.get) and added, the id of the new scene (A to H). Refused when all eight are in use. |
| [`scene.assignParam`](#sceneassignparam) | yes | edit | Gives a block parameter a value per scene, starting with its current value in all eight places; from then on block.setParam changes it in the active scene only and scene.setParamValue in any scene. With more than one scene in use block.setParam does this by itself on a parameter's first set, so calling it is only needed to assign without changing the value. Works for inputGain, outputGain, mix and the parameters of the block's kind, a hosted plugin's included. Refused for enabled (a block's on/off state is per scene already), normalize, a parameter that changes the block's latency (affectsLatency in block.listParams) and one assigned to the expression pedal. |
| [`scene.copy`](#scenecopy) | yes | edit | Copies one scene's block states and assigned values onto another, which keeps its name; copying onto the active scene is heard at once, like a scene switch. |
| [`scene.get`](#sceneget) | no | read | Returns the rig's scenes: count (how many the preset uses, 1 to 8), active (index 0 to 7) and activeId (A to H), and for each of the eight places its name, whether it is in use (inUse), whether it is the active one, blocks (the on/off state of every tone block by block id) and values (the value of each scene-assigned parameter: a knob position from 0 to 1 for inputGain, outputGain and mix, the parameter's own unit for a kind parameter). assigned lists the scene-assigned parameters and expression those set aside for the expression pedal. A parameter that is not assigned has one value shared by all scenes. |
| [`scene.remove`](#sceneremove) | yes | edit | Removes a scene that is in use. The scenes after it move down one place and keep their names, block states and assigned values; the last place goes back to a fresh scene and the count goes down by one. The sound does not change: the active scene stays active wherever it moves to. If the active scene is the one removed, the scene that takes its place is selected (the new last scene when it was the last). Refused for a scene that is not in use and for the only scene. |
| [`scene.rename`](#scenerename) | yes | edit | Names a scene (the default names are Scene A to Scene H). |
| [`scene.reset`](#scenereset) | yes | edit | Puts a scene back to its starting point: default name, every block on, and each scene-assigned parameter at the value the active scene has (resetting the active scene keeps its values). |
| [`scene.select`](#sceneselect) | yes | edit | Makes a scene the active one: its block states and assigned values are put on the blocks without a click and without muting the chain (blocks fade, a block with spillover that the scene turns off rings out, values glide), so it is safe at footswitch speed. Refused for a scene that is not in use. Not an undo step. The scene active when a rig or preset is saved is the one it opens with. |
| [`scene.setParamValue`](#scenesetparamvalue) | yes | edit | Sets a scene-assigned parameter's value in one scene without selecting that scene (for the active scene this is block.setParam). The value is clamped to the parameter's range. |
| [`scene.unassignParam`](#sceneunassignparam) | yes | edit | Takes a parameter out of the scenes again: the value the active scene has becomes the one value all scenes share. |
| [`server.setEnabled`](#serversetenabled) | yes | local | Starts or stops the command server (off by default) and keeps the choice across restarts. It listens on 127.0.0.1 only and every client must present the token from the connection file; stopping it disconnects every client. |
| [`server.status`](#serverstatus) | no | local | Returns the command server's state: enabled (the saved setting), running, port, the number of connections and the path of the connection file clients read the port and token from. |
| [`setlist.create`](#setlistcreate) | yes | edit | Creates an empty setlist (a folder under Setlists in the library) at the end of the list and returns its id. The name must not be that of another setlist. |
| [`setlist.delete`](#setlistdelete) | yes | delete | Moves a setlist's folder, presets included, to the system trash (a Trash folder in the library where there is none). Model files in Assets stay. When it was the current setlist, the first remaining one becomes current. |
| [`setlist.get`](#setlistget) | no | read | Returns one setlist: id, name, readOnly, folder, capacity (256), current, currentSlot (0 when none) and slots, the presets by slot with empty slots left out, each with slot, id, name, sceneCount, tempo (null unless the preset stores one), tags, favourite and file. ignored lists the .json files of the folder that are not presets, with the reason. A factory preset's sceneCount and tempo are null. |
| [`setlist.list`](#setlistlist) | no | read | Lists the setlists in order as { setlists: [{ id, name, readOnly, presetCount, current }], current }. The factory presets are the read-only setlist "factory", always last. |
| [`setlist.rename`](#setlistrename) | yes | edit | Renames a setlist and its folder; its id stays. |
| [`setlist.reorder`](#setlistreorder) | yes | edit | Moves a setlist to another place in the list of setlists. |
| [`setlist.select`](#setlistselect) | yes | edit | Makes a setlist the current one: the preset list, program changes, the footswitches' preset banks and the preset.* commands that take an id all follow. Loads nothing. |
| [`split.add`](#splitadd) | yes | edit | Splits a row into the row directly below it. The signal forks at splitAt, a boundary between slots (0 is before the first slot, 8 after the last): path A stays in the row, path B runs through the lower row's slots from there. With mixAt the two paths are mixed back into the row at that later boundary, aligned to the sample; without it the lower row runs to its own output or feed. The lower row must have no input of its own; its slots outside the split's range are kept and not run. One split per row. A split of a lower row has to lie inside the split above it, which makes a three-way parallel path. The lower row takes the row's format. A shape the rules forbid fails with the reason and changes nothing. |
| [`split.listParams`](#splitlistparams) | no | read | Lists the parameters of a row's split with their ranges, units, defaults and current values, and the split's id and type. |
| [`split.move`](#splitmove) | yes | edit | Moves a row's split: the fork (splitAt), the rejoin (mixAt), or both; fields left out stay. rejoin false takes the rejoin away, so the lower row runs to its own output. A position the rules forbid fails with the reason and changes nothing. |
| [`split.remove`](#splitremove) | yes | edit | Takes a row's split out. The lower row is then fed by nothing and does not run; its blocks are kept. |
| [`split.setParam`](#splitsetparam) | yes | edit | Sets one parameter of a row's split, in its real unit (split.listParams lists them): the splitter's (split.balance, split.levelA, split.levelB, split.crossoverHz, split.swap, split.stereo) or the mixer's (mix.levelA, mix.panA, mix.levelB, mix.panB, mix.invertB, mix.level, mix.mute). The value is clamped to the parameter's range and the change glides, without a gap in the audio. |
| [`split.setType`](#splitsettype) | yes | edit | Changes what a row's splitter does: balance, ab or crossover. The parameters of the other types keep their values. |
| [`state.get`](#stateget) | no | read | Returns the whole rig as one object: chain (blocks, lanes, branch, active preset; each block lists its scene-assigned parameters in sceneParams and those set aside for the expression pedal in expressionParams), params (every global parameter by id), presets (the current setlist's, as preset.list), library (as library.get), grid (as grid.get), midi (the MIDI map and the fixed default controls), scenes (as scene.get), stomps (as stomp.get), footswitches (as footswitch.getState) and engine (sample rate, block size, latency, oversampling, multi-core, version). |
| [`state.revision`](#staterevision) | no | read | Returns the chain revision, a counter that goes up when the chain changes, so a client can poll this and refetch state.get only on change (a run of knob-rate edits counts once, about 0.4 s after the last one). |
| [`state.waitUntilLoaded`](#statewaituntilloaded) | no | read | Blocks until every block has finished loading (or failed to, or stopped with a processing failure) and the rig is audible again, and applies what a parameter edit defers (the reported tail, a latency change), for headless callers that edit and then render; answers unavailable if that takes longer than timeoutMs. |
| [`stomp.assign`](#stompassign) | yes | edit | Assigns one or more blocks to a footswitch, replacing what it had; in Stomp mode a press flips each of them (so one switch can turn a block off and another on). |
| [`stomp.get`](#stompget) | no | read | Returns the rig's stomp assignments, one per footswitch A to H: the blocks the switch flips (blockIds), its label (empty: named after the first block) and whether it is momentary. Assignments are part of the rig and are saved with it and in presets. |
| [`stomp.setOptions`](#stompsetoptions) | yes | edit | Sets a footswitch's stomp label and whether it is momentary: a momentary switch flips its blocks on the press and flips them back on the release (on while held). |
| [`stomp.unassign`](#stompunassign) | yes | edit | Takes one block off a footswitch, or without blockId clears the switch entirely (its label and momentary flag too). |
| [`tempo.get`](#tempoget) | no | read | Returns the rig's own tempo as bpm (default 120), plus what the last audio callback actually ran with: effectiveBpm, and hostSync when that came from the host. |
| [`tempo.set`](#temposet) | yes | edit | Sets the rig's own tempo, which tempo-synced blocks follow whenever the host reports none (the standalone app, a host without a tempo); a host's tempo always wins. |
| [`tempo.tap`](#tempotap) | yes | edit | Counts one tap of the tempo switch and sets the rig's tempo once the taps give one: the average of the last intervals (up to four), within 20 to 300 BPM. A pause longer than 3 seconds starts a new count, a tap within 0.2 seconds of the last is dropped as a bounce, and a single interval more than 30% off the average is left out. tapped is true when this tap set the tempo (the first tap of a count and a tap that was left out do not). |
| [`toneRequest.apply`](#tonerequestapply) | yes | local | Applies the proposed tone to the rig being played (sandbox.applyToLive on the agent's sandbox, as the user: one undo step) and closes the request. This is the user's own approval, so no prompt follows. |
| [`toneRequest.claim`](#tonerequestclaim) | yes | edit | Takes on the open tone request: this connection now works on it, and the user sees its name. Refused when another connection has it. Build the tone in a sandbox of your own (sandbox.create), report steps with toneRequest.progress and finish with toneRequest.propose. |
| [`toneRequest.create`](#tonerequestcreate) | yes | local | Opens a tone request for the connected agent: what the user wants to hear, the guitar profile to build it for, a reference recording to aim for, limits and where it goes. Replaces a request that is finished or stopped; refused while one is open. |
| [`toneRequest.get`](#tonerequestget) | no | read | The tone request, open or last finished: status (waiting for an agent, working, ready with a proposal, applied, stopped), what was asked, the agent's steps, the refine conversation, and the proposal (sandbox, title, summary, changes, measured). Null when there has been none. |
| [`toneRequest.progress`](#tonerequestprogress) | yes | edit | Reports a step on the claimed request ("Adding a plate reverb"), for the user to follow; step and of give the progress (step 5 of 8). Marks the user's messages so far as seen. |
| [`toneRequest.propose`](#tonerequestpropose) | yes | edit | Hands the user the tone: the sandbox it was built in (this connection's own), a title, a summary in a sentence or two (it is also the agent's answer in the conversation), what changed as [{name, before, after}] and what was measured. The request is then ready: the user auditions it, applies it or asks for changes (toneRequest.wait sees the message). |
| [`toneRequest.reply`](#tonerequestreply) | yes | local | The user's refine message on the open request ("a bit darker, less delay in the verse"). A request that was ready goes back to working. |
| [`toneRequest.stop`](#tonerequeststop) | yes | local | Stops the open request. What the agent built so far stays in its sandbox; the agent's next step on the request is refused. |
| [`toneRequest.wait`](#tonerequestwait) | no | read | Waits until the tone request changes after `since` (the sequence a previous call returned; 0 for the first call): a new request, a refine message from the user, a stop. Returns the request and the new sequence, or the same sequence when nothing changed within the wait. An agent that serves the user's tone requests calls this in a loop. |
| [`tuner.get`](#tunerget) | no | read | Returns the tuner's latest reading as { frequency (Hz), confidence (0 to 1), level (dB) }; meaningful only while the tuner is enabled and audio is running. |
| [`tuner.setEnabled`](#tunersetenabled) | yes | edit | Starts or stops feeding the tuner from the raw input; leave it off when not tuning. |

## agents.activity

Returns the most recent commands that came through the agent layer, newest last (the last 200 are kept): time, client, command, its arguments shortened to one line, the rig it acted on and how it ended (ok, an error code, or running).

Mutates state: no. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `limit` | integer | no | 1 to 200; default `50` | How many entries to return. |

## agents.disconnect

Disconnects a client of the command server; its sandbox rigs are discarded and its requests waiting for approval are denied.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | integer | yes |  | The client's id, from agents.list. |

## agents.list

Lists the clients connected to the command server: id, the name each declared, when it connected, its last command and when, how many commands it has run and the rig it is working on (live or a sandbox id); plus the server's status.

Mutates state: no. Permission: local.

No parameters.

## approvals.list

Lists the requests waiting for the user's approval (a client asked to change the rig being played while that is switched off), each with its id, client, command and arguments; with all true also the recently resolved ones. A request to apply a sandbox carries applies: the sandbox, the revision it was captured at, when, and sandboxChanged (the sandbox is no longer the rig the request would apply, so a preview rendered through it would show something else).

Mutates state: no. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `all` | boolean | no | default `false` | Include approved, denied and expired requests. |

## approvals.resolve

Approves or denies a waiting request. Approving runs the command exactly as it was asked for and keeps its result for the client, which reads it with approvals.status. A request to apply a sandbox (sandbox.applyToLive) applies the rig the sandbox was when the request was made (applies: its sandbox, revision and capture time), whatever has been done to the sandbox since.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The request's id, from approvals.list. |
| `approve` | boolean | yes |  | True to run it, false to refuse it. |

## approvals.status

Returns one approval request: state (pending, approved, denied or expired) and, once approved, result, the envelope the command answered with. A client sees only its own requests.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The approvalId a needs_approval error carried. |

## assets.list

Lists the amp captures (.nam) and impulse responses (.wav) an agent can load with chain.addToneFromFiles: the files under the library folder and the folders the user added (source library, tags from the folder names), the library's asset store (source assets: the model files of the saved presets, under content-hash names) and the local model stash (source stash: files added before, also under content-hash names). origin says where a file came from: library, tone3000 (downloaded on the TONE3000 page, kept in the library's TONE3000 folder; listed with tone3000, the tone, creator and licence), folder (one the user added), or loaded (a copy kept for a file the user loaded). A copy kept under a hash name is listed by the name the file was loaded under (name, with loadedFrom and loadedAt, and tone3000 with the tone, creator and licence when it was downloaded from TONE3000), else by its metadata name, and query finds it by that name. A capture carries what its file says about itself as nam_name, nam_modeled_by, nam_gear_type, nam_gear_make, nam_gear_model, nam_tone_type, nam_gain and nam_loudness when present, and its architecture, architectureLabel, a2, sampleRate (when the file names the rate it was trained at) and supported (every architecture NAM builds loads; false for a file Pedaleira can not load, with unsupportedReason in a sentence); an IR its sampleRate, channels and seconds. inRig lists the tones the rig already holds, with their titles and gear tags. There is no searchable catalogue beyond this: tones from the online catalogue are only known once they are in a rig.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `kind` | string | no | one of `all`, `capture`, `ir`; default `"all"` | Only captures or only IRs. |
| `query` | string | no |  | Only assets whose name, tags or metadata contain this text (case does not matter). |
| `tags` | array | no |  | Only assets carrying every one of these tags (the user's or a folder's). |
| `untagged` | boolean | no | default `false` | Only assets the user has not tagged yet (for a review of untagged items). |
| `limit` | integer | no | 1 to 1000; default `200` | The most assets to return. |

## assets.tag

Adds tags to a capture (.nam) or IR (.wav) and removes others, in the user's own index (asset-tags.json in the library folder). Tags are kept by the file's content, so they follow it through a move or a rename. A tag is lowercased, trimmed and at most 32 characters; a file carries at most 32. Returns the file's tags after the change. See assets.tagSummary for the tags in use and a suggested vocabulary.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `file` | string (path) | yes |  | The capture or IR, an absolute path. |
| `add` | array of string | no |  | Tags to add. |
| `remove` | array of string | no |  | Tags to remove. |

## assets.tagSummary

How far the user's captures and IRs are tagged (captures and irs: total and tagged, over the same files assets.list finds), every tag in use with how many files carry it, and a suggested vocabulary to keep tags consistent (gain, character, gear, speaker, use). Agents choose tones by these tags.

Mutates state: no. Permission: read.

No parameters.

## block.exportImpulse

Writes the impulse response a cab.model block plays (docs/cab-framework.md) as a mono WAV file, so the cab can be used as an IR anywhere: the response its settings give now (speaker, box, mic, position, distance, angle, room and cuts; not its level), computed at the sample rate asked for, about 85 ms long. 16 and 24 bit files are scaled so their peak is at -1 dBFS (the answer's gainDb says by how much); a 32 bit float file keeps the level the block plays at. Only blocks of a kind that synthesises its own response accept this (cab.model).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `file` | string (path) | yes |  | The WAV file to write, an absolute path (a relative one is taken from the working directory). An existing file is replaced. |
| `sampleRate` | number | no | 8000 to 192000; default `48000.0` | The file's sample rate in Hz. |
| `bits` | integer | no | default `24` | 16 or 24 (integer) or 32 (float). |

## block.getFaceplate

How a block's controls are drawn as a faceplate, for the blocks that have one (the amp kinds; for any other kind the answer is invalid_args). It is the panel style of the block's amp description (docs/amp-framework.md, The faceplate), with the defaults filled in when the description gave none (styled false). Returns the amp's name, family (High gain, Clean, Crunch, or empty), tag and character line, the accent colour as #RRGGBB (empty: the front end's own), the nameplate position (left, right, top, bottom), the knob cap (round, pointer, chicken_head, fader), the strip pattern, the voicing names, and the groups: the boxes left to right, each with a title, its controls in order (parameter ids from block.listParams: gain, bass, middle, treble, presence, depth, master, bright, voicing, each with the label the description gave it) and the columns for its knobs (0: one per knob). Every control the amp's panel has is in exactly one group. Level is not in a group: it is the block's output knob.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |

## block.listParams

Lists everything block.setParam can set on a block, with current values: the five common settings (scope common, knob positions from 0 to 1) and then the parameters of the block's kind (scope kind, real units), each with its range, unit, default and flags. A hosted plugin block lists the plugin's own automatable parameters (the first 256) as normalised 0 to 1 values under the plugin's parameter ids, with the plugin's display text for the current value in text.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |

## block.loadDescription

Gives an amp.custom block an amp: loads an amp description file (JSON, format pedaleira-amp, see docs/amp-framework.md and docs/amp-description.schema.json) that lists the amp's parts as read off its schematic, and builds that amp. The file is checked first; if it is refused the answer is invalid_args with the loader's message, which names the field, and the block is left as it was. Otherwise the block takes the amp's name, its voicing is set to the description's default, and the description's text is kept in the block's state (saved presets do not depend on the file staying where it was). One undo step. A new amp.custom block starts as the shipped High Gain amp. A cab.model block takes a speaker description instead (format pedaleira-cab-speaker, see docs/cab-framework.md), which becomes its Custom speaker and is selected. Only blocks of a kind that takes a description (amp.custom, cab.model) accept a file.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `file` | string (path) | yes |  | The description file, an absolute path (a relative one is taken from the working directory). |

## block.resetEq

Returns a block's EQ to its flat defaults, switched on and post-model.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |

## block.setEnabled

Turns a block on or off (off passes the signal through untouched). A block of a kind with spillover (see kinds.list) whose spillover parameter is on keeps ringing after it is turned off: its input closes and its tail plays out over the dry signal, for as long as the block's reported tail (at most 30 seconds); with the parameter off the tail is cut. The state belongs to the active scene: a block is in the same state in every scene until this is first used on it, and from then on each scene has its own.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `enabled` | boolean | yes |  | True to process, false to bypass. |

## block.setEqBand

Changes one band of a block's six-band EQ; fields left out keep their current value.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `band` | integer | yes | 0 to 5 | Band index: 0 is the low band, 5 the high band, 1 to 4 are bells. |
| `type` | string | no | one of `lowcut`, `lowshelf`, `bell`, `highshelf`, `highcut` | Filter type. Band 0 takes lowcut or lowshelf, band 5 highcut or highshelf, the others are always bell; a type the band cannot take is replaced by the band's shelf or bell. |
| `freqHz` | number | no | 20 to 20000 | Centre or corner frequency in Hz. |
| `gainDb` | number | no | -15 to 15 | Gain in dB (ignored by the cut types). |
| `q` | number | no | 0.1 to 10 | Q (bandwidth or slope). |

## block.setEqEnabled

Turns a block's EQ on or off, keeping its band settings.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `enabled` | boolean | yes |  | True to apply the EQ. |

## block.setEqPre

Places a block's EQ before its model (true) or after it (false, the default).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `pre` | boolean | yes |  | True for pre-model, false for post-model. |

## block.setParam

Sets one of a block's settings. The common ones are knob positions: inputGain, outputGain and mix from 0 to 1 (0.5 is unity for the gains), enabled and normalize as switches (0 or 1). Any other id is a parameter of the block's kind (see block.listParams) and takes a value in that parameter's own unit, clamped to its range (0 to 1 for a hosted plugin's parameters). The blocks carry the active scene: enabled and a scene-assigned parameter (scene.assignParam) change in the active scene only. While the preset has more than one scene (scene.add), a parameter that is not assigned yet is scene-assigned by its first set, so it changes in the active scene only and the others keep the value they had; this is one undo step. With one scene, and for normalize, a parameter on the expression pedal or one scene.assignParam refuses, the setting has one value shared by all scenes.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `param` | string | yes |  | Which setting: enabled, normalize, inputGain, outputGain, mix, or a kind parameter id. A newly added IR block takes its default mix when its first load lands, so set mix after state.waitUntilLoaded. |
| `value` | number | yes |  | For the common settings a knob position from 0 to 1 (switches: 1 is on, 0 is off); for a kind parameter its real value (dB, ms, a choice index). |

## block.setSlimSize

Sets a NAM block's model size, trading CPU for accuracy: 0 is the lightest, 1 the full model (only slimmable captures, which NAM A2 files are, change; a capture of one size keeps it; IR blocks refuse).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `slimSize` | number | yes | 0 to 1 | Model size from 0 (lite) to 1 (full). |

## capture.addToRig

Adds the trained capture to the rig being played as a NAM block (chain.addToneFromFiles), so it can be compared with the real device. Returns what that command returns.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.cancelRecording

Stops a recording that is going and throws away what was recorded; also discards a finished recording so that it can be made again (in the record or check step).

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.cancelTraining

Stops the trainer (the child process is killed) and returns the training status.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.checkTrainer

Finds out whether the trainer can be run: starts Python and imports neural-amp-modeler, which takes a few seconds. Answers pythonFound, packageFound, versionOk (version 0.13.0 or newer, the first whose standard training makes NAM A2 files), the version and a message saying what to do when it is not usable.

Mutates state: no. Permission: local.

No parameters.

## capture.create

Starts a capture session and returns it, at the source step. Its folder is <library>/Captures/<id>; the session survives a restart.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `name` | string | yes |  | The capture's name, shown in the library. |
| `source` | string | no | one of `pedal`, `ampHead`, `ampCab`, `modeller` | What is captured: a pedal, an amp head (through a load box), an amp and cab (mic'd or through a cab sim) or a modeller. |
| `method` | string | no | one of `reamp`, `dryWet` | reamp plays the trainer's input file through the gear; dryWet records the guitar before and after the gear while you play (no re-amp box needed). |
| `gainTag` | string | no | one of `clean`, `crunch`, `highGain` | How much gain the capture has. |
| `modelSize` | string | no | one of `full`, `lite` | Full or lite. One capture file holds both; this is the size the wizard offers first. |
| `gearType` | string | no | one of `ampHead`, `ampCab`, `cabinet`, `pedal`, `outboard`, `spaces`, `experimental` | TONE3000's gear type: ampHead, ampCab, cabinet, pedal, outboard, spaces or experimental. Default: from the source. |
| `gearMake` | string | no |  | The gear's make. |
| `gearModel` | string | no |  | The gear's model. |
| `notes` | string | no |  | Notes kept with the capture (knob settings, load box). |

## capture.delete

Deletes a capture session's folder (the recordings and an unsaved model); a capture already saved to the library is a file of its own and stays. Stops a run that belongs to it.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.get

Returns one capture session: its settings, the step it is at (and what stops it from going on, blocks), levels, recording, checks, training and files. During training it carries the trainer's progress too.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.getSettings

Returns the capture settings: the Python executable the trainer runs under (empty: python3 on the path), the trainer's standard input file and whether it is the NAM project's version 3 file (by checksum), and the level target. The standard input file is published by the NAM project and is not shipped with Pedaleira.

Mutates state: no. Permission: local.

No parameters.

## capture.levelStatus

Returns the levels: the return's current peak, peak hold, RMS and clipped samples (dry/wet: both inputs), the target band and whether the hold is inside it (state silent, low, high, clipping or inRange), and the noise floor when measured. Each call also records the readings in the session, which is what lets the wizard leave the levels step.

Mutates state: no. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `resetHold` | boolean | no | default `false` | Clear the peak hold and the clip count first. |

## capture.list

Lists the capture sessions, newest first: id, name, source, method, step, training state, ESR and the saved file when there is one.

Mutates state: no. Permission: read.

No parameters.

## capture.measureNoiseFloor

Mutes the tone for a moment and measures the return's noise (RMS) over the given time. The result shows in capture.levelStatus as noiseFloor.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `seconds` | number | no | 0.5 to 10; default `2.0` | How long to measure. |

## capture.recordingStatus

Returns the recording's state (none, recording, writing, complete, failed), progress from 0 to 1, seconds recorded and left, the return's peak so far, clipped samples and, for a re-amp, the latency measured once enough has come in.

Mutates state: no. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.review

Returns what the review step shows: the trained file, the ESR and its band (great under 0.01, not bad under 0.035, might be okay under 0.1, poor under 0.3, else failed), the calibration and the metadata the saved file will carry.

Mutates state: no. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.runChecks

Runs the data checks on the finished recording and returns the session with the report (checks): length, peak level against the target band, clipping, noise floor, latency, DC offset and, for dry/wet, that both inputs have signal and the wet is not a copy of the dry. Each check is pass, warn or fail; the verdict is the worst. Moves a capture at the record step on to the check step.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.save

Saves the capture into the library: the trained file moves to <library>/Captures/<name>.nam (a copy with the name, gear, gain tag and calibration set in its metadata when the trainer did not write them). Returns the file.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.setChannels

Sets which device channels the capture uses, counted from 0: for a re-amp the send output and the return input, for dry/wet the dry and wet inputs. Also the calibration: how many dBu 0 dBFS at the return input is (saved in the capture as output_level_dbu; the Volt 876 is +12.5) and, when known, at the send output (input_level_dbu). The processor has the main stereo pair only for now, so 0 and 1 are the usable channels; the answer warns about a channel the device does not have.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `send` | integer | no | 0 to 7 | Re-amp: the output channel the signal goes out on. |
| `returnIn` | integer | no | 0 to 7 | Re-amp: the input channel the gear comes back on. |
| `dry` | integer | no | 0 to 7 | Dry/wet: the input with the guitar before the gear. |
| `wet` | integer | no | 0 to 7 | Dry/wet: the input with the gear. |
| `calibrationDbu` | number | no | -40 to 60 | dBu at 0 dBFS on the return input. |
| `sendCalibrationDbu` | number | no | -40 to 60 | dBu at 0 dBFS on the send output. |

## capture.setLevels

Sets the send level of the test tone and of the signal that is recorded (its peak, in dBFS), the level target and band the return has to reach, and for dry/wet how long to record. The target defaults to the capture settings' level target, the band to 3 dB either side. Changing the send level or the target clears the peak readings.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `sendDb` | number | no | -80 to 0 | The send level, dBFS. |
| `targetDb` | number | no | -30 to -1 | The peak the return should reach, dBFS. |
| `bandDb` | number | no | 0.5 to 6 | How far either side of the target counts as in range. |
| `recordSeconds` | number | no | 10 to 600 | Dry/wet: how long to record (default 180). |

## capture.setSettings

Sets any of the capture settings and returns them all (capture.getSettings).

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `pythonExecutable` | string (path) | no |  | The Python that has neural-amp-modeler 0.13.0 or newer installed; empty to use python3 on the path. |
| `standardInputFile` | string (path) | no |  | Absolute path of the NAM standard input file (v3_0_0.wav, also called input.wav); empty to clear. |
| `levelTargetDb` | number | no | -30 to -1 | The peak level a capture should reach, in dBFS; new captures start with it. TONE3000 recommends -8, the design -12 (the default). |

## capture.setSource

Changes what the capture is: its name, source, method, gain tag, model size, gear and notes. The source and the method can only change up to the wiring step. Returns the session.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `name` | string | no |  | The capture's name. |
| `source` | string | no | one of `pedal`, `ampHead`, `ampCab`, `modeller` | What is captured: a pedal, an amp head (through a load box), an amp and cab (mic'd or through a cab sim) or a modeller. |
| `method` | string | no | one of `reamp`, `dryWet` | reamp plays the trainer's input file through the gear; dryWet records the guitar before and after the gear while you play (no re-amp box needed). |
| `gainTag` | string | no | one of `clean`, `crunch`, `highGain` | How much gain the capture has. |
| `modelSize` | string | no | one of `full`, `lite` | Full or lite. One capture file holds both; this is the size the wizard offers first. |
| `gearType` | string | no | one of `ampHead`, `ampCab`, `cabinet`, `pedal`, `outboard`, `spaces`, `experimental` | TONE3000's gear type: ampHead, ampCab, cabinet, pedal, outboard, spaces or experimental. Default: from the source. |
| `gearMake` | string | no |  | The gear's make. |
| `gearModel` | string | no |  | The gear's model. |
| `notes` | string | no |  | Notes kept with the capture (knob settings, load box). |

## capture.setStep

Moves the wizard to a step: one forward, when what the current step needs holds (the answer of a refused move says what is missing), or any step back, which throws away what the steps left behind had made (going back to record discards the recording). Steps: source, wiring, levels, record, check, train, review.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `step` | string | yes | one of `source`, `wiring`, `levels`, `record`, `check`, `train`, `review` | The step to go to. |
| `overrideChecks` | boolean | no | default `false` | Going on from the check step although the checks failed. |

## capture.startRecording

Starts the recording in the record step. Re-amp: the trainer's standard input file (capture.setSettings) plays on the send channel at the send level and the return is recorded, sample for sample with what is played; it takes about three minutes. Dry/wet: both inputs are recorded together while you play, for the recording time. The audio device must run at 48 kHz. Poll capture.recordingStatus; the files are written when it ends.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `allowUnrecognisedInput` | boolean | no | default `false` | Use a standard input file whose checksum is not the NAM project's version 3 (the trainer is likely to refuse it). |

## capture.startTone

Starts the levels run in the levels step: the rig is muted, a 1 kHz test tone plays on the send channel at the send level and the return is metered (for dry/wet there is no tone: the two inputs are metered while the guitar is played). Poll capture.levelStatus. Standalone application only.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `sendDb` | number | no | -80 to 0 | The send level, dBFS; default: the session's. |

## capture.startTraining

Starts the trainer (neural-amp-modeler 0.13.0 or newer, whose standard training makes NAM A2 files) as a child process and returns at once. From the check step, the checks must not have failed unless overrideChecks is set. Poll capture.trainingStatus. Training an already trained capture again (from review) starts over.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |
| `epochs` | integer | no | 1 to 2000 | Training epochs (the trainer's default is 100). |
| `overrideChecks` | boolean | no | default `false` | Train although our data checks failed. |
| `ignoreTrainerChecks` | boolean | no | default `false` | Tell the trainer to go on although its own data checks fail (at your own risk). |
| `latency` | integer | no | -10000 to 10000 | Re-amp: the delay in samples, instead of the trainer's own calibration. |

## capture.stopTone

Ends the levels run (the readings are kept in the session) and unmutes the rig.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## capture.trainingStatus

Returns the training state (none, checking, running, done, failed, cancelled), the epoch and progress, the time left, the validation ESR with its band (great, notBad, mightBeOkay, poor, failed), the latency the trainer used, whether its own data checks passed, the last lines of its output, and an error message when it failed. A finished training whose file is not NAM A2 is a failure. Done moves the capture to review.

Mutates state: no. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The capture's id, from capture.create or capture.list. |

## chain.addBlock

Adds a block of a registered kind (see kinds.list), such as utility.gain, at its default settings; unlike a tone it has no model to load and is live when the command returns.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `kind` | string | yes |  | The kind id, from kinds.list. |
| `target` | string | no |  | Block id to put it in: an empty insert slot (the block takes that position) or an existing tone block (replaced in place, keeping its id, gains, mix and EQ). Default: the first empty slot of the lane. |
| `lane` | string | no | one of `left`, `right` | Lane to add to when no target is given; right needs stereo mode. Default: the lane last added to (chain.activeSide). |
| `row` | integer | no | 0 to 3 | Grid mode: the row of the slot to put it in, with slot, instead of target or lane. |
| `slot` | integer | no | 0 to 7 | Grid mode: the slot in that row. |

## chain.addPlugin

Adds a block that hosts one of the scanned effect plugins and returns it; the plugin is created on the message thread after the command returns, so the block shows as loading until state.waitUntilLoaded (or a poll of state.get) sees it loaded, and its parameters (block.listParams) exist from then on. A plugin that will not load leaves a bypassed placeholder under its name.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `identifier` | string (path) | yes |  | The plugin: an identifier from plugins.list (status ok), or the plugin's file path or exact name when that names exactly one effect. |
| `target` | string | no |  | An empty insert slot's block id (the block takes that position). Default: the first empty slot of the lane. |
| `lane` | string | no | one of `left`, `right` | Lane to add to when no target is given; right needs stereo mode. Default: the lane last added to (chain.activeSide). |
| `row` | integer | no | 0 to 3 | Grid mode: the row of the slot to put it in, with slot, instead of target or lane. |
| `slot` | integer | no | 0 to 7 | Grid mode: the slot in that row. |

## chain.addToneFromFiles

Adds ONE tone block from local files and starts loading it in the background: a single .nam or .wav (IR) file, a folder of them, or several files of the same kind that become the selectable models of that one block (call once per block to add an amp and a cab).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `paths` | array of string (path) | yes |  | Absolute paths. One entry may be a file or a folder; several entries must all be .nam files or all be .wav files. |
| `target` | string | no |  | Block id to load into: an empty insert slot (the tone takes that position) or an existing tone block (its tone is replaced in place, keeping its id and settings). Default: the first empty slot of the lane. |
| `lane` | string | no | one of `left`, `right` | Lane to add to when no target is given; right needs stereo mode. Default: the lane last added to (chain.activeSide). |
| `row` | integer | no | 0 to 3 | Grid mode: the row of the slot to put it in, with slot, instead of target or lane. They name the slot as target would: an empty slot takes the tone, a block already in it has its tone replaced in place. |
| `slot` | integer | no | 0 to 7 | Grid mode: the slot in that row. |
| `tone3000` | object | no |  | Where the files came from on TONE3000, kept with the block, its presets and the recent list so the creator and the licence stay shown: {id, title, url, gear, format, license, image, capture, user: {username, display_name, avatar_url, is_verified}}. It needs a title or a user; other fields are dropped. |

## chain.clearBranch

Removes the branch so both lanes take their own input again.

Mutates state: yes. Permission: edit.

No parameters.

## chain.duplicateBlock

Clones a tone block with its model and every setting, and returns the new block's id.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block to clone. |
| `side` | string | no | one of `left`, `right` | Lane the clone lands in. Default: the source's lane. |
| `index` | integer | no | at least 0 | Position in that lane, counting insert slots; an empty slot there is filled, anything else is pushed along. Default: right after the source. |
| `row` | integer | no | 0 to 3 | Grid mode: the row the clone lands in, with slot, instead of side and index (an empty slot there is filled, anything else is pushed toward the row's nearest empty slot). |
| `slot` | integer | no | 0 to 7 | Grid mode: the slot in that row. |

## chain.moveBlock

Moves a tone block across to the other lane at the given position (stereo mode only; use chain.reorder to move within a lane).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block to move. |
| `side` | string | yes | one of `left`, `right` | The lane to move it into. |
| `index` | integer | yes | at least 0 | Position in the destination lane, counting insert slots; clamped to its length. |

## chain.redo

Redoes the chain edit most recently undone.

Mutates state: yes. Permission: edit.

No parameters.

## chain.removeBlock

Removes a tone block from the chain.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block to remove. |

## chain.reorder

Reorders the blocks of one lane; signal flows through them in the order given.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `order` | array of string | yes |  | Every block id of exactly one lane, empty insert slots included, in the new order (the lane is inferred from the ids; see left / right in any chain command's result). |

## chain.reset

Returns to the factory default: an empty mono chain, preset-managed parameters at their defaults and no active preset, as one undoable step.

Mutates state: yes. Permission: edit.

No parameters.

## chain.setBranch

Feeds one lane from the other lane's signal after a chosen tone block instead of from its own input (stereo mode only); calling it again moves the tap.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `side` | string | yes | one of `left`, `right` | The trunk lane, which keeps its own input. |
| `afterBlockId` | string | yes |  | A tone block in that lane (the trunk); the other lane taps its output. |

## chain.setInputMode

Chooses how a stereo source feeds the chain: stereo (the natural routing), left or right (one channel onto both), or dual (a mono chain run once per channel).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `mode` | string | yes | one of `stereo`, `left`, `right`, `dual` | The input mode. |

## chain.setStereo

Switches between one mono chain and two independent left / right lanes.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `enabled` | boolean | yes |  | True for two lanes, false for one chain. |

## chain.swapLanes

Swaps the left and right lanes wholesale (stereo mode only).

Mutates state: yes. Permission: edit.

No parameters.

## chain.undo

Undoes the last chain edit (structure, tones, block settings, EQ, stereo mode, preset load); global parameters are not part of the history.

Mutates state: yes. Permission: edit.

No parameters.

## commands.describe

Lists every command with its summary, parameters and whether it mutates state, or describes the one named.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `name` | string | no |  | A command name; leave out for the full list. |

## events.subscribe

Turns change notifications on or off for the calling connection. With revision on, the server sends {"event":"revision","rig":...,"revision":N} whenever the revision of the rig the connection is working on (the live rig or its selected sandbox) changes, so a client can refetch state.get without polling.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `revision` | boolean | no | default `true` | Send revision events. |

## expression.assignParam

Sets a block parameter aside for the expression pedal. The pedal itself is not there yet; what the flag does today is keep the parameter out of the scenes: it is unassigned from them (the active scene's value stays as the shared one) and scene.assignParam refuses it, so a scene switch never pulls a pedal-driven value away.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `param` | string | yes |  | The parameter: inputGain, outputGain, mix, or a parameter of the block's kind (see block.listParams). |

## expression.unassignParam

Clears a parameter's expression pedal flag, so it can be scene-assigned again.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `param` | string | yes |  | The parameter: inputGain, outputGain, mix, or a parameter of the block's kind (see block.listParams). |

## favourite.set

Marks a preset as a favourite or takes the mark away. Favourites are a set of preset ids kept in library.json; a factory preset can be one.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Preset id. |
| `favourite` | boolean | yes |  | Whether it is a favourite. |

## footswitch.getState

Returns what a pedalboard view draws: mode (preset, scene, stomp or hybrid), hybridRows (what the top row A to D and the bottom row E to H do in hybrid mode), bank and bankCount (preset banks), scenePage, activeScene, sceneCount (scenes in use), bpm, and the eleven switches (A to H, bankUp, bankDown, tempo), each with its role in the current mode (preset, scene, stomp, bankUp, bankDown, presetNext, presetPrevious or tapTempo), label, colour (a hint: the category of a stomp's first block, else the role), lit, held, empty (nothing behind the switch, also a scene the preset does not use) and its target (scene, presetId and presetIndex, or blockIds and momentary).

Mutates state: no. Permission: read.

No parameters.

## footswitch.press

Presses a footswitch. A to H act by mode: Preset loads preset bank x 8 + switch of the preset list, Scene selects scene A to H, Stomp flips the switch's assigned blocks, Hybrid does per row what footswitch.setHybridRows says. bankUp and bankDown move the preset bank (and, in a hybrid with one scene row, flip that row between scenes A to D and E to H); where there is nothing to page (Scene and Stomp mode) they step to the next or previous preset. tempo taps the tempo (see tempo.tap). Follow it with footswitch.release.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `switch` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H`, `bankUp`, `bankDown`, `tempo` | The footswitch: A to H, bankUp, bankDown or tempo. |

## footswitch.release

Releases a footswitch; a momentary stomp flips its blocks back, nothing else acts on the release.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `switch` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H`, `bankUp`, `bankDown`, `tempo` | The footswitch: A to H, bankUp, bankDown or tempo. |

## footswitch.setHybridRows

Sets what each row does in hybrid mode: scene, stomp or preset. A stomp row uses its own switches' assignments. With both rows on scenes the top row is scenes A to D and the bottom E to H; a single scene row shows A to D and bankUp / bankDown flip it to E to H. A preset row shows four presets, two show eight, and the bank switches move by that many. Machine-wide, like the mode.

Mutates state: yes. Permission: settings.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `top` | string | yes | one of `scene`, `stomp`, `preset` | Switches A to D. |
| `bottom` | string | yes | one of `scene`, `stomp`, `preset` | Switches E to H. |

## footswitch.setMode

Sets what the eight footswitches do: preset, scene, stomp or hybrid. A machine-wide setting, kept across rigs, presets and restarts.

Mutates state: yes. Permission: settings.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `mode` | string | yes | one of `preset`, `scene`, `stomp`, `hybrid` | The footswitch mode. |

## grid.convert

Converts the rig between the legacy routing (one chain, or two lanes with an optional branch) and the grid, as one undo step. To grid: lane position i becomes slot i of row 0 or 1, and lossy lists what does not carry over. A branch becomes a split of row 0 into row 1 that does not rejoin (A/B, both at 0 dB), with the trunk as row 0. To legacy: only a grid with the shape of a legacy rig converts (one stereo or dual-mono row, two mono rows, or two mono rows joined by such a split, on inputs 1 and 2 and output pair 1); otherwise it fails with the reasons.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `to` | string | yes | one of `grid`, `legacy` | The routing to convert to. |

## grid.get

Returns the rig's routing: mode (legacy or grid), legacyProjectable (whether a grid could be written as a legacy rig), and in grid mode rows (each with index, format, input, output, slots as the eight block ids of the row with empty slots included, active per slot (a block that is run: in a routed row and, for the lower row of a split, inside the split's range), activeSlots as [first, end), routed, source (device, split, rows or none), sink (device, row, mix or none), runsAs (the format the row runs in), latencySamples and padSamples at the chain rate, cpu, inputMissing and outputFallback), splits (each with id, row, lowerRow, splitAt, mixAt or null, type, active, lowerActive, params by id, alignment as the samples paths a and b wait at the mixer, and meters), feeds (each from, to, active and padSamples), groups (the rows that are processed together: tied by a split or a feed), latencySamples (as reported to the host), chainLatencySamples, cpu (total and critical, the plan's estimate as fractions of one core), imageStage (none in grid mode), legacyControls (per legacy image control, rows when rows 0 and 1 carry it and held when it rests at neutral and does nothing) and io (the device's input channels and output pairs, each with whether it is available; a channel or pair a row asks for that the device lacks is listed as unavailable).

Mutates state: no. Permission: read.

No parameters.

## grid.moveBlock

Moves a block to a slot of the grid. swap exchanges it with whatever is there (a block or an empty slot); insert puts it there and pushes the blocks from that slot on toward the row's nearest empty slot, and fails when the row is full. A NAM block that lands in a row of another format (mono or stereo against dual mono) is rebuilt for it.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The block to move. |
| `row` | integer | yes | 0 to 3 | The row, 0 (top) to 3. |
| `slot` | integer | yes | 0 to 7 | The slot in that row, 0 to 7. |
| `mode` | string | no | one of `swap`, `insert`; default `"swap"` | What happens to a block already there. |

## guitar.analyze

Measures every take of a guitar profile again and rebuilds its summary (after clips were replaced on disk, or when the measurements changed with a new version); returns the profile, with unreadable listing take files that could not be read.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `guitar` | string | yes |  | The guitar's id. |

## guitar.create

Creates a guitar profile and returns it; the first one becomes the default. Clips are added afterwards with guitar.importClip or guitar.record.start.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `name` | string | yes |  | The guitar's name. |
| `positions` | array of string | yes |  | The pickup positions clips will be recorded for, for example bridge, middle, neck (1 to 8). |
| `pickupType` | string | no | one of `humbucker`, `single-coil`, `p90`, `active`, `piezo`, `other`; default `"other"` | The kind of pickups. |
| `tuning` | string | no | default `""` | The tuning, as free text (E standard, drop D). |
| `notes` | string | no | default `""` | Anything else worth knowing. |

## guitar.delete

Deletes a guitar profile with its clips (to the system trash where there is one) and returns the remaining list.

Mutates state: yes. Permission: delete.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `guitar` | string | yes |  | The guitar's id. |

## guitar.get

Returns one guitar profile in full: name, pickupType, positions, tuning, notes, takes (each a recorded DI clip of one position, soft, hard, chords or notes, with its measurements: peakDb, rmsDb, lufs, crestDb, centroidHz, the band balance, lowEndShare, hum at 50 and 60 Hz and the noise floor) and the summary. Tones are built for a guitar by rendering its clips (render.clip).

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `guitar` | string | yes |  | The guitar's id, from guitar.list. |

## guitar.importClip

Attaches an existing WAV file to a guitar profile as one take of one position (copied into the profile, replacing a take already there), measures it and returns the profile. The clip should be the dry DI signal of the guitar, mono, a few seconds long.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `guitar` | string | yes |  | The guitar's id. |
| `position` | string | yes |  | The pickup position, as the profile names it (for example bridge). |
| `take` | string | yes | one of `soft`, `hard`, `chords`, `notes` | Which take: soft picking, hard picking, chords or single notes. |
| `path` | string (path) | yes |  | The WAV file, as an absolute path inside the allowed folders (see roots.list); at most 120 seconds. |

## guitar.list

Lists the guitar profiles: id, name, pickup type, positions, tuning, how many takes each has, its summary (outputLevel low/medium/high, brightness dark/balanced/bright, noise quiet/moderate/noisy, with the numbers behind them) and which one is the default.

Mutates state: no. Permission: read.

No parameters.

## guitar.record.cancel

Stops a take recording that is in progress and discards what was recorded.

Mutates state: yes. Permission: edit.

No parameters.

## guitar.record.start

Starts recording a take from the live audio input: the raw signal of one input channel, before the rig, for the given number of seconds, starting now. Poll guitar.record.status; when the time is up the take is saved into the profile and measured. Replaces a recording in progress. It listens to what the user is playing, so for a command server client it counts as changing the live rig (approval).

Mutates state: yes. Permission: live.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `guitar` | string | yes |  | The guitar's id. |
| `position` | string | yes |  | The pickup position, as the profile names it (for example bridge). |
| `take` | string | yes | one of `soft`, `hard`, `chords`, `notes` | Which take: soft picking, hard picking, chords or single notes. |
| `seconds` | number | no | 1 to 60; default `8.0` | How long to record. |
| `channel` | integer | no | 0 to 7; default `0` | The device input channel the guitar is on, counted from 0. |

## guitar.record.status

Returns the take recording's state: idle, recording (with recordedSeconds, progress from 0 to 1 and peakDb, the input's peak so far), writing (the buffer is full and the take is being saved and measured), done (with result, the new take and its measurements) or failed (with error).

Mutates state: no. Permission: read.

No parameters.

## guitar.setDefault

Makes a guitar the default one: the one render.clip uses when none is named.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `guitar` | string | yes |  | The guitar's id. |

## guitar.update

Changes a guitar profile's name, pickup type, positions, tuning or notes; fields left out stay. A position that has takes cannot be removed.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `guitar` | string | yes |  | The guitar's id. |
| `name` | string | no |  | The guitar's name. |
| `positions` | array of string | no |  | The pickup positions. |
| `pickupType` | string | no | one of `humbucker`, `single-coil`, `p90`, `active`, `piezo`, `other` | The kind of pickups. |
| `tuning` | string | no |  | The tuning. |
| `notes` | string | no |  | Notes. |

## kinds.list

Lists every block kind this build knows: id, name, category, the family it is filed under within its category and a line about it (family and blurb: every shipped amp is a kind of its own, filed under High gain, Clean or Crunch with its character line; empty for the other kinds), whether chain.addBlock can create it (nam and ir blocks come from chain.addToneFromFiles instead), the mix a new block of it starts at (defaultMix), how its mix knob blends dry and wet (mixLaw: crossfade is dry x (1 - mix) + wet x mix; dryPreserving keeps the dry signal at unity up to mix 0.5, where the wet signal reaches unity too, and fades it above that), whether its tail rings out after a bypass (spillover: the kind then has a spillover switch among its parameters, on by default) its own parameters with their ranges, units and defaults, and its readings (read-only values it reports while it runs, such as a compressor's gain reduction; meters.get returns them). categories names every category a kind can be filed under.

Mutates state: no. Permission: read.

No parameters.

## library.get

Returns the library: root (its folder), setlists (as setlist.list), current (setlist, slot or 0, and presetId, the active preset's id), favourites (preset ids), recents (as recent.list), unsaved (the rig differs from the active preset; with no active preset, from the empty default) and legacyPresets (how many presets of the old .t3kpreset store library.importLegacyPresets would bring over).

Mutates state: no. Permission: read.

No parameters.

## library.importLegacyPresets

Copies the user presets of the old .t3kpreset store into a setlist as JSON presets, keeping their ids. The setlist is made when missing and a second one is started when it is full. The old files are neither moved nor deleted, and presets imported before are skipped. (An empty library brings the old preset list over by itself the first time it is used; this is for presets deleted since, or a library that was not empty.) Returns { imported, skipped, failed, setlists }.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `setlist` | string | no | default `"Imported Presets"` | Name of the setlist to import into. |

## library.rescan

Reads the library's folders again from scratch, for changes made outside the app (a file browser, git) that the usual time and size check could miss. With tidy the preset files are also renamed to what their slots and names make them (a file added without a slot number gets one). Returns library.get plus renamed.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `tidy` | boolean | no | default `false` | Also rename the preset files. |

## looper.clear

Erases the loop and all its layers and goes back to empty.

Mutates state: yes. Permission: edit.

No parameters.

## looper.get

Returns the looper's state as the audio thread last published it: state (empty, arming, recording, playing, overdubbing or stopped; arming is a record action waiting for the loop's memory, which is reserved on the first record and freed after the loop has been empty for a while), memoryReady, memoryBytes and memoryFailed (the reservation was refused), closing (the first recording is running on to the next beat, with quantise on), queued (actions waiting for the next audio block), channels (1 or 2), lengthFrames and lengthSeconds (of the loop as recorded), playbackLengthSeconds (twice that at half speed), recordedSeconds (while recording), positionFrames, positionSeconds and position (0 to 1) in the loop, layers (the recording and the overdubs in the sound now; the one being recorded counts), canUndo, redoLayers, canRedo, layersFull, maxLayers, maxSeconds, capacitySeconds, and options: halfSpeed, reverse, oneShot, quantise and levelDb. The loop is not saved with a preset or the plugin state.

Mutates state: no. Permission: read.

No parameters.

## looper.overdub

Layers new playing on top of the loop (up to four layers after the first recording). Recording: ends the first recording and starts overdubbing from the top of the loop. Playing or stopped: starts overdubbing. Overdubbing: ends the layer and keeps playing. Nothing is recorded in half speed or reverse: overdubbing runs at normal speed, forwards. Ignored with no loop, and when all layers are used (layersFull).

Mutates state: yes. Permission: edit.

No parameters.

## looper.play

Plays the loop. Stopped: from the start (or, in reverse, from the end). Recording: ends the first recording and plays it. Overdubbing: ends the layer and keeps playing.

Mutates state: yes. Permission: edit.

No parameters.

## looper.record

Records the loop from the main output (after the tone stack, before the output level). Empty: starts the first recording, which sets the loop's length when it ends. Recording: ends it and starts playing (with quantise on, at the nearest whole number of beats). With a loop already there: throws it away and starts a new recording. Up to 60 seconds; at the limit the loop closes by itself. The first record after startup (or after the memory was freed) reserves the loop's memory off the audio thread: the state reads queued, then arming, and recording starts in the first block after the memory is there (a few milliseconds). Takes effect at the start of the next audio block. Answers with looper.get's state, which does not show the action yet.

Mutates state: yes. Permission: edit.

No parameters.

## looper.redo

Puts back the overdub layer looper.undo took out, if no new overdub has started since.

Mutates state: yes. Permission: edit.

No parameters.

## looper.setOptions

Sets how the loop plays; give at least one. halfSpeed plays at half speed, an octave down and twice as long (playbackLengthSeconds); reverse plays backwards; oneShot plays the loop once and stops instead of repeating; quantise rounds the length of the first recording to a whole number of beats at the tempo of the moment it ends; levelDb is the loop's playback level (the rig's own sound is not touched). They take effect within a block and can change while playing. Answers with looper.get's state.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `halfSpeed` | boolean | no |  | Half speed playback. |
| `reverse` | boolean | no |  | Reverse playback. |
| `oneShot` | boolean | no |  | Play once, then stop. |
| `quantise` | boolean | no |  | Round the first recording's length to whole beats (applies to the next one that ends). |
| `levelDb` | number | no | -60 to 12 | Playback level in dB, -60 (silent) to 12. |

## looper.stop

Stops the loop and puts it back at the start, keeping what was recorded. Recording: ends the first recording and stops. Overdubbing: ends the layer first.

Mutates state: yes. Permission: edit.

No parameters.

## looper.undo

Takes the last overdub layer out of the sound (the first recording stays; use looper.clear for that). While overdubbing it throws away the layer being recorded and keeps playing. The layer is kept for looper.redo until a new overdub starts.

Mutates state: yes. Permission: edit.

No parameters.

## meters.get

Returns the current levels in dB (floor -60): input and output as [left, right], each block's in and out by block id (with readings, by reading id, for a kind that has them: see kinds.list), the audio thread's CPU load (0 to 1) and the output's stereo correlation (-1 to 1). Also rows (in grid mode, per row what entered it and what it sent, as [left, right]), splits (in grid mode, per split what the splitter sent down paths A and B as split and what reached the mixer from each as mix), outputs (per output pair in use, its level after the output strip) and planCpu (total, critical and rows: what the rig's blocks are estimated to cost, as fractions of one core; critical is the most one thread has to carry with multi-core on).

Mutates state: no. Permission: read.

No parameters.

## midi.getMap

Returns the MIDI map: the channel filter (0 is omni), each mapping's target, source (cc or note) and number, and defaults, the fixed controls (scene select, tap tempo, tuner, footswitch mode and the fourteen looper controls, CC 48 to 61) each with its action, cc and whether a mapping of the user's own overrides it.

Mutates state: no. Permission: read.

No parameters.

## midi.remove

Removes the mapping of one target.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `targetId` | string | yes |  | What the control drives: a global parameter id, block1Power to block64Power (the Nth tone block of the left lane), rightBlock1Power and up (right lane), stereoEnabled, presetPrevious or presetNext. |

## midi.setCc

Maps a MIDI CC number to a target, replacing any mapping that target already had.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `targetId` | string | yes |  | What the control drives: a global parameter id, block1Power to block64Power (the Nth tone block of the left lane), rightBlock1Power and up (right lane), stereoEnabled, presetPrevious or presetNext. |
| `cc` | integer | yes | 0 to 127 | Controller number. |

## midi.setChannelFilter

Limits MIDI control (mappings and program changes) to one channel, or to all of them.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `channel` | integer | yes | 0 to 16 | 1 to 16 for one channel, 0 for omni. |

## nam.clearRecent

Empties the list nam.recent returns. The files stay findable by name with assets.list.

Mutates state: yes. Permission: local.

No parameters.

## nam.downloaded

Finds the file a capture of a TONE3000 tone was downloaded to before, in the library's TONE3000 folder: {path} when it is there, {path: ""} when it is not (never downloaded, or the file was deleted). The TONE3000 page asks before it downloads, so a capture is fetched once.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `tone` | integer | yes | 1 to 2147483647 | The TONE3000 tone id. |
| `capture` | integer | yes | 1 to 2147483647 | The TONE3000 capture (model) id. |

## nam.inspect

Tells whether Pedaleira can load a NAM capture, without loading it: its architecture (architecture as the file says it, architectureLabel as a player knows it, such as WaveNet A2, WaveNet A1, LSTM, ConvNet, Linear or Slimmable A2), a2 (true for NAM A2), sampleRate (the rate it was trained at, when the file says; it always runs at that rate, resampled when the session runs at another, and at 48 kHz when the file does not say), supported (every architecture NAM builds loads; false for a file that is not a capture, a NAM version or architecture Pedaleira cannot read, or damaged settings) and, when it is not supported, unsupportedReason in a sentence that can be shown as it is. Weights that do not fit the settings are only found when the file is loaded. modelName is the name in the file's metadata, empty when it has none.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `path` | string (path) | yes |  | A .nam file, as an absolute path. |

## nam.keepDownload

Keeps a capture or IR just downloaded from TONE3000 in the library: moves `file` into <library>/TONE3000 under a name made from the tone's title and the capture's name ("Title - Capture.nam", unsafe characters replaced, a number added when the name is taken) and remembers which tone and capture it is and who made it, so it persists across restarts, is found by assets.list (origin tone3000, with its tone3000 credit) and is reused by nam.downloaded. Returns {path}, the kept file. The file is then loaded with chain.addToneFromFiles.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `file` | string (path) | yes |  | The downloaded .nam or .wav, an absolute path. It is moved. |
| `capture` | integer | yes | 1 to 2147483647 | The TONE3000 capture (model) id. |
| `tone3000` | object | yes |  | Where it came from: {id (the tone), title, url, gear, format, license, image, capture (the capture's name), user: {username, display_name, avatar_url, is_verified}}, as chain.addToneFromFiles takes it. |

## nam.recent

Lists the captures (or IRs) the player loaded from a file, newest first: name (the file name without its extension), path (the file to load again: the original when it is still there, else the copy the model stash kept), fromStash (true for that copy), loadedAt (milliseconds since 1970), for a capture what nam.inspect says, and tone3000 (the tone, creator and licence on TONE3000) for a file that came from there. A file that is gone from both places is left out. The stash drops copies that were not used for a week, so an old entry whose original moved away can disappear.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `kind` | string | no | one of `all`, `capture`, `ir`; default `"capture"` | Captures, IRs or both. |
| `limit` | integer | no | 1 to 200; default `30` | The most entries to return. |

## param.get

Returns one global parameter in the same shape as param.list.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Parameter id, for example gateThreshold. |

## param.list

Returns every global (faceplate and settings) parameter by id with its value in real units, normalized value, range, default, name, type and unit.

Mutates state: no. Permission: read.

No parameters.

## param.set

Sets one global parameter to a value in real units (dB, Hz, a choice index, 0 or 1 for a switch), clamped to the parameter's range, and returns the parameter as it now is.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Parameter id, for example gateThreshold. |
| `value` | number | yes |  | The new value in the parameter's own units; out-of-range values are clamped. |

## permissions.get

Returns what command server clients may do, one switch per permission class: read (library, presets and rig state), edit (create and edit presets and rigs), render (render clips to measure a tone), live (change the rig being played; off by default, and then such a request waits for approval), delete (delete presets, setlists and guitars; off by default) and settings (audio, device and machine-wide settings; off by default).

Mutates state: no. Permission: read.

No parameters.

## permissions.set

Changes the permission switches for command server clients; switches left out stay as they are. Kept across restarts.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `read` | boolean | no |  | Read library, presets and rig state. |
| `edit` | boolean | no |  | Create and edit presets and rigs. |
| `render` | boolean | no |  | Render audio clips to measure a tone. |
| `live` | boolean | no |  | Change the rig being played without asking. |
| `delete` | boolean | no |  | Delete presets and guitar profiles. |
| `settings` | boolean | no |  | Change audio, device and machine-wide settings. |

## plugins.addFolder

Adds a folder to search for plugins on top of the formats' default locations (kept across sessions) and returns the added folders; the next plugins.scan looks there.

Mutates state: yes. Permission: settings.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `path` | string (path) | yes |  | The folder, as an absolute path. |

## plugins.cancelScan

Stops a running plugin scan after the plugin it is on and returns the scan status; what was scanned so far is kept.

Mutates state: yes. Permission: settings.

No parameters.

## plugins.list

Lists every plugin the scanner knows with its name, vendor, format, category, version, channels, file and the identifier chain.addPlugin takes, and a status: ok (an effect that can be added), failed scan, not an effect (instruments and MIDI-only plugins) or missing (no longer found); also the added folders and the scan status.

Mutates state: no. Permission: read.

No parameters.

## plugins.removeFolder

Removes a folder added with plugins.addFolder and returns the added folders; plugins that were only found there show as missing after the next plugins.scan.

Mutates state: yes. Permission: settings.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `path` | string (path) | yes |  | The folder, as an absolute path. |

## plugins.scan

Starts a scan for VST3 and Audio Unit plugins in the background, replacing one that is running, and returns the scan status; each plugin is scanned in a helper process, so one that crashes or hangs is blacklisted with the reason instead of taking the app down, and files that have not changed since the last scan are not scanned again. Answers unavailable when the helper executable is not installed.

Mutates state: yes. Permission: settings.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `folders` | array of string (path) | no |  | Scan these folders only (absolute paths) instead of the formats' default locations plus the added folders; Audio Units, which have no folders, are then skipped unless formats names them. |
| `files` | array of string (path) | no |  | Scan exactly these plugin files and search nowhere: absolute paths (a .vst3) with no .. in them, which for a command server client must lie inside the allowed folders. A local caller may also give Audio Unit identifiers (AudioUnit:Effects/aufx,dely,appl). Takes precedence over folders. |
| `formats` | array of string | no |  | Formats to scan, by name: VST3, and AudioUnit on macOS. Default: every format this build hosts. |
| `rescanAll` | boolean | no | default `false` | Scan unchanged files again as well, the blacklisted ones included. |
| `waitMs` | integer | no | 0 to 3600000; default `0` | Wait up to this long for the scan to finish before answering (for scripts); the scan carries on in the background if it takes longer. |

## plugins.scanStatus

Returns the plugin scan's progress: state (idle, scanning, finished, cancelled or unavailable), the file being scanned, done and total candidates, and how many needed scanning and failed.

Mutates state: no. Permission: read.

No parameters.

## preset.copyToSetlist

Copies a preset into another setlist under a new id, into the given empty slot or the first empty one. The source may be a factory preset; model files are shared, not copied.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `slot` | integer | yes | 1 to 256 | The slot to copy. |
| `to` | string | yes |  | Id of the setlist the copy goes to. |
| `toSlot` | integer | no | 1 to 256 | The empty slot the copy goes to. |
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## preset.delete

Moves a preset's file to the system trash and leaves its slot empty (factory presets cannot be deleted); the loaded chain is left as it is.

Mutates state: yes. Permission: delete.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Preset id, as preset.list returns it. |

## preset.deleteSlot

Moves the preset file of a slot to the system trash and leaves the slot empty; the other presets keep their slots and the loaded chain is left as it is.

Mutates state: yes. Permission: delete.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `slot` | integer | yes | 1 to 256 | The slot to empty. |
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## preset.duplicateSlot

Copies a preset within its setlist under a new id, as "<name> copy", into the given empty slot or the first empty slot after it.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `slot` | integer | yes | 1 to 256 | The slot to copy. |
| `to` | integer | no | 1 to 256 | The empty slot the copy goes to. |
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## preset.exportJson

Writes a preset as a JSON file at any path: the preset with the given id, or without an id the rig as it is now. Model files and large plugin states go into an Assets folder beside the file (the layout preset.importJson reads), or into the file itself as base64 with embedAssets.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `path` | string (path) | yes |  | The file to write. |
| `id` | string | no |  | Preset id; the current rig when omitted. |
| `name` | string | no |  | A name for the exported preset. |
| `embedAssets` | boolean | no | default `false` | One self-contained file. |

## preset.importForeign

Imports a preset read from another product (a foreign preset JSON, docs/import.md; tools/import/quadcortex_dump.py writes them from a Quad Cortex) into an empty slot of a setlist, as a grid preset: its blocks become ours through a mapping table (the built-in one for the file's source, or the one at `mapping`), rows and splits keep their layout as far as the grid allows, and scenes carry what the scene model can hold. Amps, cabs and captures are never mapped: their slots stay empty for the player to fill. Returns the new preset as preset.importJson does, plus report: { mapped, approximated, unmapped, droppedParams, notes }, each entry naming the block by row, position and the source's model. Loads nothing.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `file` | string (path) | yes |  | The foreign preset JSON to read. |
| `mapping` | string (path) | no |  | A mapping table to use instead of the built-in one for the file's source. |
| `slot` | integer | no | 1 to 256 | The empty slot it goes to. |
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## preset.importJson

Reads a JSON preset (or a .t3kpreset) from any path into an empty slot of a setlist: the given one or the first empty one. Its assets are taken from the file, from an Assets folder beside it or from the library, and stored in the library; ones nobody has are listed in missingAssets and the preset is imported all the same. Loads nothing.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `path` | string (path) | yes |  | The file to read. |
| `slot` | integer | no | 1 to 256 | The empty slot it goes to. |
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## preset.list

Lists the presets of the current setlist in slot order as { presets: [{ id, name, factory, slot }], setlist: { id, name, readOnly } }. Empty slots are left out; a preset's slot is also its MIDI program number plus one and its place on the footswitches' preset banks.

Mutates state: no. Permission: read.

No parameters.

## preset.load

Loads a preset, replacing the chain and the preset-managed parameters, and starts loading its models in the background (one undoable step for the chain). The id may be of any setlist's preset (a favourite, a recent); the current setlist only changes through setlist.select and preset.loadSlot. A block whose model file is missing from the library comes up failed under its name and the rest of the rig loads.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Preset id, as preset.list returns it. |

## preset.loadSlot

Loads the preset in a slot (as preset.load does) and makes its setlist the current one.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `slot` | integer | yes | 1 to 256 | The slot, 1 to 256. |
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## preset.move

Moves a preset up or down among the presets of the current setlist: the slots in use stay the same and the presets trade places in them, which also sets the order MIDI program changes follow. preset.moveSlot moves one to a slot by number.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Preset id, as preset.list returns it. |
| `delta` | integer | yes | -1000 to 1000 | Steps to move: negative is earlier, positive later. |

## preset.moveSlot

Moves a preset to another slot of its setlist and renumbers the files. insert (the default) shifts the presets and gaps in between one slot towards where it came from; swap exchanges the two slots.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `from` | integer | yes | 1 to 256 | The slot the preset is in. |
| `to` | integer | yes | 1 to 256 | The slot it goes to. |
| `mode` | string | no | one of `insert`, `swap`; default `"insert"` | What happens to the slots in between. |
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## preset.previewForeign

The dry run of preset.importForeign: reads and converts a foreign preset the same way, with the same mapping table, and returns what the import would make without saving or loading anything: { name, report }, the report as preset.importForeign returns it. Every block of the file appears in exactly one of the report's mapped, approximated (type block) or unmapped lists, so a caller can show what each block becomes before importing.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `file` | string (path) | yes |  | The foreign preset JSON to read. |
| `mapping` | string (path) | no |  | A mapping table to use instead of the built-in one for the file's source. |

## preset.rename

Renames a preset of the user's and its file; its id and slot stay (factory presets cannot be renamed).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Preset id, as preset.list returns it. |
| `name` | string | yes |  | The new name; must not be blank. |

## preset.save

Saves the current chain and preset-managed parameters as a preset of the current setlist, makes it the active preset and returns its id, setlist and slot. Saving under the name of a preset of the setlist replaces that preset; otherwise it goes into the first empty slot. With the read-only factory setlist current it goes to the first setlist of the user's own ("My Presets" is made when there is none), which becomes current.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `name` | string | yes |  | Preset name; must not be blank. |

## preset.saveToSlot

Saves the current chain and preset-managed parameters into a slot of a setlist, makes that the active preset and the setlist the current one. A preset already in the slot is replaced and keeps its id; name may then be omitted to keep its name too.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `slot` | integer | yes | 1 to 256 | The slot, 1 to 256. |
| `name` | string | no |  | Preset name; required for an empty slot. |
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## recent.list

Lists the presets loaded last, most recent first (at most 32), as { recents: [{ id, name, setlist, slot, factory }] }. Presets that no longer exist are left out.

Mutates state: no. Permission: read.

No parameters.

## render.clip

Plays recorded guitar clips through a rig offline and returns measurements of what came out, one entry in renders per clip (and per scene): input (the clip's own peak, RMS and LUFS), levels (peakDb, truePeakDb, rmsDb, crestDb, lufs), gainDb (output RMS minus input RMS), spectrum (the share of energy in sub, bass, lowMid, mid, highMid, presence and air, in dB and as fractions, the spectral centroid and the low-end share), stereo (correlation, width, balanceDb), distortion (percent: how much of the output a linear filter of the input cannot explain; time effects longer than the fit and modulation count too, so bypass them to measure the amp alone), tail (seconds the output keeps sounding after the clip ends) and latency. dynamics pairs each position's soft and hard takes: their distortion, cleanupRatio (soft over hard) and compressionDb (how much of the playing dynamics the rig removes). The rig is the connection's selected sandbox, the one named, or a private copy of the live rig; the rig being played is never rendered through. The same call on the same rig returns the same numbers. A sandbox cannot be edited while it renders, and at most two renders run at a time in the whole application: a third answers unavailable.

Mutates state: no. Permission: render.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `guitar` | string | no |  | The guitar profile whose clips to play (see guitar.list). Default: the default guitar. |
| `position` | string | no |  | Only that pickup position's takes. |
| `takes` | array of string | no |  | Only these takes: soft, hard, chords, notes. Default: all the guitar has. |
| `clips` | array of string (path) | no |  | Instead of a guitar: WAV files to play, as absolute paths inside the allowed folders (see roots.list); mono or stereo, at most 120 seconds each. One call renders at most 600 seconds in all (clips and tails, once per scene). |
| `scenes` | array of string | no |  | Render once per scene named here (A to H); the rig returns to its current scene afterwards. Default: the current scene only. |
| `tailSeconds` | number | no | 0 to 30; default `2.0` | How long to keep rendering after a clip ends, to catch delay and reverb tails. |
| `sandbox` | string | no |  | The sandbox rig to render through. Default: the calling connection's selected sandbox, or a private copy of the live rig. |
| `writeAudio` | boolean | no | default `false` | Also write each render as a WAV into the scratch folder and return its path as audioPath. |

## render.compare

Measures a reference recording and the rig's render of the chosen clips (all of them joined) the same way and reports how far apart they are: reference and rig (levels, spectrum, stereo), difference (rig minus reference: lufs, crestDb, bandsDb per band, centroidOctaves, correlation, width) and distance, a score per group (loudness, dynamics, tone, stereo) and overall, where 0 is identical and 1 is a difference a listener would not miss (3 dB of loudness or crest factor, 3 dB RMS across the bands or a third of an octave of centroid, 0.3 of correlation). Takes the clip arguments of render.clip.

Mutates state: no. Permission: render.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `reference` | string (path) | yes |  | The recording to aim at: a WAV file, as an absolute path inside the allowed folders, mono or stereo, at most 120 seconds. |
| `guitar` | string | no |  | The guitar profile whose clips to play (see guitar.list). Default: the default guitar. |
| `position` | string | no |  | Only that pickup position's takes. |
| `takes` | array of string | no |  | Only these takes: soft, hard, chords, notes. Default: all the guitar has. |
| `clips` | array of string (path) | no |  | Instead of a guitar: WAV files to play, as absolute paths inside the allowed folders (see roots.list); mono or stereo, at most 120 seconds each. One call renders at most 600 seconds in all (clips and tails, once per scene). |
| `scenes` | array of string | no |  | Render once per scene named here (A to H); the rig returns to its current scene afterwards. Default: the current scene only. |
| `tailSeconds` | number | no | 0 to 30; default `2.0` | How long to keep rendering after a clip ends, to catch delay and reverb tails. |
| `sandbox` | string | no |  | The sandbox rig to render through. Default: the calling connection's selected sandbox, or a private copy of the live rig. |

## render.probe

Measures a rig with a test signal instead of a guitar clip. response: plays noise at levelDb and returns the rig's gain per frequency (points: hz, gainDb, and coherence, which falls below 1 where the rig is not behaving linearly at that level) plus the overall distortion percent; meaningful for linear-ish rigs or low levels. levelCurve: plays a sine at frequencyHz in steps from -60 to 0 dBFS and returns the output level for each (points: inputDb, outputDb, gainDb, all as sine peak levels), which shows compression and gain staging. noise: plays silence and returns the noise floor (noiseFloorDb RMS, peakDb) and any mains hum. Uses the rig render.clip would.

Mutates state: no. Permission: render.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `kind` | string | yes | one of `response`, `levelCurve`, `noise` | The probe to run. |
| `levelDb` | number | no | -80 to -6; default `-30.0` | response: the noise level, dBFS RMS. |
| `frequencyHz` | number | no | 40 to 8000; default `1000.0` | levelCurve: the sine's frequency. |
| `frequencies` | array of number | no |  | response: the frequencies to report, in Hz. Default: third octaves from 25 Hz to 16 kHz. |
| `sandbox` | string | no |  | The sandbox rig to probe. Default: the calling connection's selected sandbox, or a private copy of the live rig. |

## roots.add

Adds a folder to the ones clients may read files from (captures, IRs, reference recordings). Kept across restarts.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `path` | string (path) | yes |  | An existing folder, as an absolute path. |

## roots.list

Lists the folders a command server client may name in a path argument (roots): the library, the local model stash, the scratch folder and the ones the user added (added). A path anywhere else is refused.

Mutates state: no. Permission: read.

No parameters.

## roots.remove

Removes a folder added with roots.add.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `path` | string (path) | yes |  | The folder, as roots.list shows it. |

## row.setFormat

Sets what a row's buffer carries: mono (one channel, panned onto its output), stereo (a stereo pair) or dual (two independent mono signals through the same blocks, each amp running once per channel). Amps in the row rebuild when the change moves them between one and two voices.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 3 | The row, 0 (top) to 3. |
| `format` | string | yes | one of `mono`, `stereo`, `dual` | The row format. |

## row.setInput

Sets where a row's signal comes from: none (the row is not fed and does not run; its blocks are kept) or a device channel, or the pair starting at it. A pair can be summed to mono. An input the device does not have is silence and is reported as inputMissing; the setting is kept for a device that has it.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 3 | The row, 0 (top) to 3. |
| `source` | string | yes | one of `none`, `device` | The kind of input. |
| `channel` | integer | no | 1 to 8; default `1` | The device's input channel, counted from 1. |
| `stereo` | boolean | no | default `false` | Take the pair: channel and the one after it. |
| `fold` | string | no | one of `none`, `sum`; default `"none"` | For a pair: none keeps left and right apart, sum puts half of their sum on both. |

## row.setInputStage

Sets a row's share of the input stage; fields left out stay as they are. trimDb is the row's own level on top of the rig's input level (applied as the signal enters the row, after the gate). gate switches the noise gate for the row's input (the gate's settings are the rig's). pitch asks for the pitch shifter: one input runs it, the first that asks, and the others are delayed to stay in step.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 3 | The row, 0 (top) to 3. |
| `trimDb` | number | no | -24 to 24 | Input trim in dB. |
| `gate` | boolean | no |  | Run the noise gate on this row's input. |
| `pitch` | boolean | no |  | Run the pitch shifter on this row's input. |

## row.setMix

Sets a row's send to its output: volume, pan, mute, solo and polarity; fields left out stay as they are. Pan is constant power on a mono row (hard left or right is unity) and a balance on a stereo or dual-mono row (unity at the centre, the far side falling to silence). Solo silences the outputs of the rows that are neither soloed nor fed by a soloed row. A row that feeds another row goes in at its volume, mute and polarity; its pan and solo wait for a device output. Changes glide, without a gap in the audio.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 3 | The row, 0 (top) to 3. |
| `volumeDb` | number | no | -60 to 12 | Volume in dB; the bottom of the range is off. |
| `pan` | number | no | 0 to 1 | 0 is left, 0.5 the centre, 1 right. |
| `mute` | boolean | no |  | Silence the row's send. |
| `solo` | boolean | no |  | Hear only the soloed rows. |
| `invert` | boolean | no |  | Flip the row's polarity. |

## row.setOutput

Sets where a row's signal goes: none (the row does not run), a pair of device outputs, or another row (target row with toRow), whose input it is then summed into at the row's volume, with the rows that feed one row kept in step. A fed row has no input of its own and is not the lower row of a split, and feeds form no loop; otherwise the command fails with the reason and nothing changes. A pair the device does not have falls back to pair 1 and is reported as outputFallback; the setting is kept.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 3 | The row, 0 (top) to 3. |
| `target` | string | yes | one of `none`, `device`, `row` | The kind of output. |
| `pair` | integer | no | 1 to 4; default `1` | The output pair, counted from 1 (pair 2 is Out 3/4). |
| `toRow` | integer | no | 0 to 3 | For target row: the row to feed. |

## sandbox.applyToLive

Copies a sandbox rig onto the rig being played, the way loading a preset would: the chain with its scenes and the preset-managed parameters, as one undo step (chain.undo on the live rig brings the previous chain back). The sandbox is kept. This is the step that changes what the user hears: with the live permission off it answers needs_approval and runs when the user approves; what is applied then is the sandbox's rig as it was when this was called, so edit first and ask after (asking again after an edit replaces the earlier request). Default: the calling connection's selected sandbox.

Mutates state: yes. Permission: live.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `sandbox` | string | no |  | The sandbox id. |

## sandbox.create

Creates a sandbox rig: a second rig that is never heard, for building and measuring a tone without disturbing the one being played. It starts as a copy of the live rig (from live), empty, or from a preset, and is selected for the calling connection unless use is false: from then on every rig command of that connection (chain.*, block.*, scene.*, param.*, preset.*, state.* and the rest) acts on the sandbox instead of the live rig. A copy loads its blocks in the background like a project being opened, so call state.waitUntilLoaded before editing them. A sandbox belongs to the connection that made it: no other client can select, edit, render, apply or discard it. At most four exist at a time; a connection's sandboxes are discarded when it closes.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `from` | string | no | one of `live`, `empty`, `preset`; default `"live"` | What the sandbox starts as. |
| `presetId` | string | no |  | With from preset: the preset's id, from preset.list. |
| `name` | string | no |  | A label for sandbox.list. |
| `use` | boolean | no | default `true` | Select it for this connection. |

## sandbox.discard

Discards one of the calling connection's sandbox rigs and frees what it held; the connection goes back to the live rig if it had it selected. Default: the calling connection's selected sandbox.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `sandbox` | string | no |  | The sandbox id. |

## sandbox.list

Lists the calling connection's own sandbox rigs (a local caller sees every client's): id, name, the client that made it, whether a render is running through it and whether it is the calling connection's selected rig; selected names that rig (live when none).

Mutates state: no. Permission: read.

No parameters.

## sandbox.use

Selects the rig the calling connection's rig commands act on: one of its own sandboxes by id, or live for the rig being played.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `sandbox` | string | yes |  | A sandbox id from sandbox.list, or live. |

## scene.add

Adds the next scene (a preset starts with one and can have up to eight). The new scene starts as a copy of the active scene's block states and assigned values, with its default name; the active scene stays the same. Returns the scenes (as scene.get) and added, the id of the new scene (A to H). Refused when all eight are in use.

Mutates state: yes. Permission: edit.

No parameters.

## scene.assignParam

Gives a block parameter a value per scene, starting with its current value in all eight places; from then on block.setParam changes it in the active scene only and scene.setParamValue in any scene. With more than one scene in use block.setParam does this by itself on a parameter's first set, so calling it is only needed to assign without changing the value. Works for inputGain, outputGain, mix and the parameters of the block's kind, a hosted plugin's included. Refused for enabled (a block's on/off state is per scene already), normalize, a parameter that changes the block's latency (affectsLatency in block.listParams) and one assigned to the expression pedal.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `param` | string | yes |  | The parameter: inputGain, outputGain, mix, or a parameter of the block's kind (see block.listParams). |

## scene.copy

Copies one scene's block states and assigned values onto another, which keeps its name; copying onto the active scene is heard at once, like a scene switch.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `from` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The scene to copy, A to H. |
| `to` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The scene to overwrite, A to H. |

## scene.get

Returns the rig's scenes: count (how many the preset uses, 1 to 8), active (index 0 to 7) and activeId (A to H), and for each of the eight places its name, whether it is in use (inUse), whether it is the active one, blocks (the on/off state of every tone block by block id) and values (the value of each scene-assigned parameter: a knob position from 0 to 1 for inputGain, outputGain and mix, the parameter's own unit for a kind parameter). assigned lists the scene-assigned parameters and expression those set aside for the expression pedal. A parameter that is not assigned has one value shared by all scenes.

Mutates state: no. Permission: read.

No parameters.

## scene.remove

Removes a scene that is in use. The scenes after it move down one place and keep their names, block states and assigned values; the last place goes back to a fresh scene and the count goes down by one. The sound does not change: the active scene stays active wherever it moves to. If the active scene is the one removed, the scene that takes its place is selected (the new last scene when it was the last). Refused for a scene that is not in use and for the only scene.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `scene` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The scene to remove, A to H. |

## scene.rename

Names a scene (the default names are Scene A to Scene H).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `scene` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The scene to name, A to H. |
| `name` | string | yes |  | The new name; empty restores the default. |

## scene.reset

Puts a scene back to its starting point: default name, every block on, and each scene-assigned parameter at the value the active scene has (resetting the active scene keeps its values).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `scene` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The scene to reset, A to H. |

## scene.select

Makes a scene the active one: its block states and assigned values are put on the blocks without a click and without muting the chain (blocks fade, a block with spillover that the scene turns off rings out, values glide), so it is safe at footswitch speed. Refused for a scene that is not in use. Not an undo step. The scene active when a rig or preset is saved is the one it opens with.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `scene` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The scene to select, A to H. |

## scene.setParamValue

Sets a scene-assigned parameter's value in one scene without selecting that scene (for the active scene this is block.setParam). The value is clamped to the parameter's range.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `scene` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The scene to change, A to H. |
| `blockId` | string | yes |  | The tone block. |
| `param` | string | yes |  | The parameter: inputGain, outputGain, mix, or a parameter of the block's kind (see block.listParams). |
| `value` | number | yes |  | A knob position from 0 to 1 for inputGain, outputGain and mix; a kind parameter's own unit otherwise. |

## scene.unassignParam

Takes a parameter out of the scenes again: the value the active scene has becomes the one value all scenes share.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `blockId` | string | yes |  | The tone block. |
| `param` | string | yes |  | The parameter: inputGain, outputGain, mix, or a parameter of the block's kind (see block.listParams). |

## server.setEnabled

Starts or stops the command server (off by default) and keeps the choice across restarts. It listens on 127.0.0.1 only and every client must present the token from the connection file; stopping it disconnects every client.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `enabled` | boolean | yes |  | True to start listening, false to stop. |
| `port` | integer | no | 0 to 65535 | The port to listen on; 0 picks a free one. Default: the port used last time, or a free one. |

## server.status

Returns the command server's state: enabled (the saved setting), running, port, the number of connections and the path of the connection file clients read the port and token from.

Mutates state: no. Permission: local.

No parameters.

## setlist.create

Creates an empty setlist (a folder under Setlists in the library) at the end of the list and returns its id. The name must not be that of another setlist.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `name` | string | yes |  | Setlist name; must not be blank. |
| `select` | boolean | no | default `false` | Make it the current setlist. |

## setlist.delete

Moves a setlist's folder, presets included, to the system trash (a Trash folder in the library where there is none). Model files in Assets stay. When it was the current setlist, the first remaining one becomes current.

Mutates state: yes. Permission: delete.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Setlist id. |

## setlist.get

Returns one setlist: id, name, readOnly, folder, capacity (256), current, currentSlot (0 when none) and slots, the presets by slot with empty slots left out, each with slot, id, name, sceneCount, tempo (null unless the preset stores one), tags, favourite and file. ignored lists the .json files of the folder that are not presets, with the reason. A factory preset's sceneCount and tempo are null.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `setlist` | string | no |  | Setlist id, as setlist.list returns it; the current setlist when omitted. |

## setlist.list

Lists the setlists in order as { setlists: [{ id, name, readOnly, presetCount, current }], current }. The factory presets are the read-only setlist "factory", always last.

Mutates state: no. Permission: read.

No parameters.

## setlist.rename

Renames a setlist and its folder; its id stays.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Setlist id. |
| `name` | string | yes |  | The new name; must not be blank. |

## setlist.reorder

Moves a setlist to another place in the list of setlists.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Setlist id. |
| `index` | integer | yes | 0 to 9999 | Its new place, 0 for the first. |

## setlist.select

Makes a setlist the current one: the preset list, program changes, the footswitches' preset banks and the preset.* commands that take an id all follow. Loads nothing.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | Setlist id, or "factory". |

## split.add

Splits a row into the row directly below it. The signal forks at splitAt, a boundary between slots (0 is before the first slot, 8 after the last): path A stays in the row, path B runs through the lower row's slots from there. With mixAt the two paths are mixed back into the row at that later boundary, aligned to the sample; without it the lower row runs to its own output or feed. The lower row must have no input of its own; its slots outside the split's range are kept and not run. One split per row. A split of a lower row has to lie inside the split above it, which makes a three-way parallel path. The lower row takes the row's format. A shape the rules forbid fails with the reason and changes nothing.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 2 | The row that is split (the upper one), 0 to 2; the row below it is the lower path. |
| `splitAt` | integer | yes | 0 to 8 | The boundary the signal forks at, 0 to 8. |
| `mixAt` | integer | no | 1 to 8 | The boundary the paths rejoin at, above splitAt and up to 8. Left out: no rejoin. |
| `type` | string | no | one of `balance`, `ab`, `crossover`; default `"balance"` | The splitter: balance (both paths at unity, one knob fades either), ab (a level per path) or crossover (lows to A, highs to B). |

## split.listParams

Lists the parameters of a row's split with their ranges, units, defaults and current values, and the split's id and type.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 2 | The row that is split (the upper one), 0 to 2; the row below it is the lower path. |

## split.move

Moves a row's split: the fork (splitAt), the rejoin (mixAt), or both; fields left out stay. rejoin false takes the rejoin away, so the lower row runs to its own output. A position the rules forbid fails with the reason and changes nothing.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 2 | The row that is split (the upper one), 0 to 2; the row below it is the lower path. |
| `splitAt` | integer | no | 0 to 8 | The boundary the signal forks at, 0 to 8. |
| `mixAt` | integer | no | 1 to 8 | The boundary the paths rejoin at, up to 8. |
| `rejoin` | boolean | no |  | false: no rejoin (mixAt is then not allowed). |

## split.remove

Takes a row's split out. The lower row is then fed by nothing and does not run; its blocks are kept.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 2 | The row that is split (the upper one), 0 to 2; the row below it is the lower path. |

## split.setParam

Sets one parameter of a row's split, in its real unit (split.listParams lists them): the splitter's (split.balance, split.levelA, split.levelB, split.crossoverHz, split.swap, split.stereo) or the mixer's (mix.levelA, mix.panA, mix.levelB, mix.panB, mix.invertB, mix.level, mix.mute). The value is clamped to the parameter's range and the change glides, without a gap in the audio.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 2 | The row that is split (the upper one), 0 to 2; the row below it is the lower path. |
| `param` | string | yes |  | The parameter id. |
| `value` | number | yes |  | The value in the parameter's unit; 0 or 1 for a switch. |

## split.setType

Changes what a row's splitter does: balance, ab or crossover. The parameters of the other types keep their values.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `row` | integer | yes | 0 to 2 | The row that is split (the upper one), 0 to 2; the row below it is the lower path. |
| `type` | string | yes | one of `balance`, `ab`, `crossover` | The splitter type. |

## state.get

Returns the whole rig as one object: chain (blocks, lanes, branch, active preset; each block lists its scene-assigned parameters in sceneParams and those set aside for the expression pedal in expressionParams), params (every global parameter by id), presets (the current setlist's, as preset.list), library (as library.get), grid (as grid.get), midi (the MIDI map and the fixed default controls), scenes (as scene.get), stomps (as stomp.get), footswitches (as footswitch.getState) and engine (sample rate, block size, latency, oversampling, multi-core, version).

Mutates state: no. Permission: read.

No parameters.

## state.revision

Returns the chain revision, a counter that goes up when the chain changes, so a client can poll this and refetch state.get only on change (a run of knob-rate edits counts once, about 0.4 s after the last one).

Mutates state: no. Permission: read.

No parameters.

## state.waitUntilLoaded

Blocks until every block has finished loading (or failed to, or stopped with a processing failure) and the rig is audible again, and applies what a parameter edit defers (the reported tail, a latency change), for headless callers that edit and then render; answers unavailable if that takes longer than timeoutMs.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `timeoutMs` | integer | no | 0 to 600000; default `30000` | How long to wait, in milliseconds. |

## stomp.assign

Assigns one or more blocks to a footswitch, replacing what it had; in Stomp mode a press flips each of them (so one switch can turn a block off and another on).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `switch` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The footswitch, A to H. |
| `blockIds` | array of string | yes |  | The tone blocks the switch flips, at least one. |
| `label` | string | no |  | What to call the switch; see stomp.setOptions. |
| `momentary` | boolean | no |  | On while held; see stomp.setOptions. |

## stomp.get

Returns the rig's stomp assignments, one per footswitch A to H: the blocks the switch flips (blockIds), its label (empty: named after the first block) and whether it is momentary. Assignments are part of the rig and are saved with it and in presets.

Mutates state: no. Permission: read.

No parameters.

## stomp.setOptions

Sets a footswitch's stomp label and whether it is momentary: a momentary switch flips its blocks on the press and flips them back on the release (on while held).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `switch` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The footswitch, A to H. |
| `label` | string | no |  | What to call the switch; empty names it after its first block. |
| `momentary` | boolean | no |  | True for on while held, false to latch. |

## stomp.unassign

Takes one block off a footswitch, or without blockId clears the switch entirely (its label and momentary flag too).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `switch` | string | yes | one of `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H` | The footswitch, A to H. |
| `blockId` | string | no |  | The block to take off; leave out to clear the switch. |

## tempo.get

Returns the rig's own tempo as bpm (default 120), plus what the last audio callback actually ran with: effectiveBpm, and hostSync when that came from the host.

Mutates state: no. Permission: read.

No parameters.

## tempo.set

Sets the rig's own tempo, which tempo-synced blocks follow whenever the host reports none (the standalone app, a host without a tempo); a host's tempo always wins.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `bpm` | number | yes | 20 to 300 | Beats per minute. |

## tempo.tap

Counts one tap of the tempo switch and sets the rig's tempo once the taps give one: the average of the last intervals (up to four), within 20 to 300 BPM. A pause longer than 3 seconds starts a new count, a tap within 0.2 seconds of the last is dropped as a bounce, and a single interval more than 30% off the average is left out. tapped is true when this tap set the tempo (the first tap of a count and a tap that was left out do not).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `timeMs` | number | no | at least 0 | When the tap happened, in milliseconds on any clock that moves forward (a remote's own timestamp, so network jitter does not blur the tempo). Default: now. |

## toneRequest.apply

Applies the proposed tone to the rig being played (sandbox.applyToLive on the agent's sandbox, as the user: one undo step) and closes the request. This is the user's own approval, so no prompt follows.

Mutates state: yes. Permission: local.

No parameters.

## toneRequest.claim

Takes on the open tone request: this connection now works on it, and the user sees its name. Refused when another connection has it. Build the tone in a sandbox of your own (sandbox.create), report steps with toneRequest.progress and finish with toneRequest.propose.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The request's id. |

## toneRequest.create

Opens a tone request for the connected agent: what the user wants to hear, the guitar profile to build it for, a reference recording to aim for, limits and where it goes. Replaces a request that is finished or stopped; refused while one is open.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `prompt` | string | yes |  | What the user wants to hear. |
| `guitar` | string | no |  | The guitar profile's id. |
| `reference` | string (path) | no |  | A recording to aim for, an absolute path. |
| `limits` | object | no |  | Limits the agent keeps to (blocks, CPU, kinds). |
| `target` | string | no | one of `newPreset`, `currentPreset`, `emptyRows`; default `"newPreset"` | Where the tone goes when applied. |

## toneRequest.get

The tone request, open or last finished: status (waiting for an agent, working, ready with a proposal, applied, stopped), what was asked, the agent's steps, the refine conversation, and the proposal (sandbox, title, summary, changes, measured). Null when there has been none.

Mutates state: no. Permission: read.

No parameters.

## toneRequest.progress

Reports a step on the claimed request ("Adding a plate reverb"), for the user to follow; step and of give the progress (step 5 of 8). Marks the user's messages so far as seen.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The request's id. |
| `text` | string | yes |  | What the agent is doing. |
| `step` | integer | no | 0 to 1000 | This step's number. |
| `of` | integer | no | 0 to 1000 | How many steps there will be. |

## toneRequest.propose

Hands the user the tone: the sandbox it was built in (this connection's own), a title, a summary in a sentence or two (it is also the agent's answer in the conversation), what changed as [{name, before, after}] and what was measured. The request is then ready: the user auditions it, applies it or asks for changes (toneRequest.wait sees the message).

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `id` | string | yes |  | The request's id. |
| `sandbox` | string | yes |  | The sandbox holding the tone. |
| `summary` | string | yes |  | What was built, for the user. |
| `title` | string | no |  | A name for the tone. |
| `changes` | array of object | no |  | What changed: [{name, before, after}]. |
| `measured` | object | no |  | What was measured (render.clip readings). |

## toneRequest.reply

The user's refine message on the open request ("a bit darker, less delay in the verse"). A request that was ready goes back to working.

Mutates state: yes. Permission: local.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `text` | string | yes |  | The message. |

## toneRequest.stop

Stops the open request. What the agent built so far stays in its sandbox; the agent's next step on the request is refused.

Mutates state: yes. Permission: local.

No parameters.

## toneRequest.wait

Waits until the tone request changes after `since` (the sequence a previous call returned; 0 for the first call): a new request, a refine message from the user, a stop. Returns the request and the new sequence, or the same sequence when nothing changed within the wait. An agent that serves the user's tone requests calls this in a loop.

Mutates state: no. Permission: read.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `since` | integer | no | default `0` | The sequence last seen. |
| `timeoutMs` | integer | no | 0 to 25000; default `20000` | How long to wait. |

## tuner.get

Returns the tuner's latest reading as { frequency (Hz), confidence (0 to 1), level (dB) }; meaningful only while the tuner is enabled and audio is running.

Mutates state: no. Permission: read.

No parameters.

## tuner.setEnabled

Starts or stops feeding the tuner from the raw input; leave it off when not tuning.

Mutates state: yes. Permission: edit.

| Parameter | Type | Required | Allowed | Description |
|---|---|---|---|---|
| `enabled` | boolean | yes |  | True to run the pitch detector. |

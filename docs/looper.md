# The looper

A loop recorded from the rig's output and played back into it. Engine and
command layer only; there is no screen for it yet. Source: `plugin/include/Looper.h`,
`plugin/src/Looper.cpp` (engine), `plugin/src/LooperCommands.cpp` (commands),
`MidiMapper` (CC 48 to 61).

## Where it sits

At the end of the main output, in `runOutputStrip`: after the tone stack, before
the auto-align mute and the output level (the master). It records what the rig
plays at that point and adds the loop to it, so the output level, the meters and
the mute treat the loop like any other sound. The rig's own sound passes
untouched. The loop is not saved with presets or plugin state. Other output
buses (a grid's extra outputs) have no looper.

## Design

**Memory.** The loop needs five layers of two channels of 60 seconds each at the
host rate: 115 MB of address space at 48 kHz. It is reserved lazily. `prepare()`
(from `prepareToPlay`, message thread) only records the rate and starts the
looper's worker thread, an idle thread that sleeps on an event. The first
`record` or `cycle` action asks the worker for the memory (`request()` signals it;
the MIDI door, on the audio thread, sets a flag the worker polls every 10 ms).
The worker `malloc`s the blocks, never writes them, so the system commits only the
pages a loop actually uses, and swaps them in under a try-lock. Meanwhile the
looper is **arming**: the record action waits, the rig's sound passes untouched,
`looper.get` reports `state: "arming"` (and, before the audio thread has picked the
action up, `queued: 1`), and recording starts in the first block after the
handover (a few milliseconds). `stop` or `clear` while arming take the action
back; a second `record` changes nothing. When the loop has been empty (a `clear`,
say) with nothing queued for the grace period (30 s), the worker takes the memory
back; the next record arms again. The audio thread allocates and frees nothing:
it only tries the lock at the start of a block, and a block that meets it (the
worker holds it for a pointer swap or a `free`, and only while the looper is
empty) passes the sound through. Calling `prepareToPlay` again at the same rate
keeps the loop and its memory; a new rate drops both. `looper.get` reports
`memoryReady`, `memoryBytes` and `memoryFailed` (the allocation was refused; the
arming record is dropped).

**Layers.** Layer 0 is the first recording, covering the whole loop. An overdub
records into the next layer (up to four). Each layer remembers the range of loop
frames it covers (`start`, `count`), and playback reads a layer only inside that
range, so a layer that is reused never needs clearing and an overdub that
covered half the loop sounds over that half only. Undo takes the last layer out
of the sum (`active`), redo puts it back; a new overdub drops what could still
be redone. Playback is the sum of the layers in use, times the level, times a
raised-cosine fade over the first and last 2 ms of the loop.

**Length.** Fixed when the first recording ends: the frames recorded, or, with
quantise on, the nearest whole number of beats at the tempo of that moment
(rounding down cuts the recording; rounding up keeps recording until the beat,
`closing` in `looper.get`). A recording that reaches 60 seconds closes itself.

**State machine.**

| State | record | overdub | play | stop | undo | redo | clear |
|---|---|---|---|---|---|---|---|
| empty | start recording | | | | | | |
| recording | close, play | close, overdub | close, play | close, stopped | | | empty |
| playing | new recording (replaces) | overdub | | stopped | drop last layer | put it back | empty |
| overdubbing | new recording | end layer, play | end layer, play | end layer, stopped | throw the layer away, play | | empty |
| stopped | new recording | play from the top, overdub | play from the top | | drop last layer | put it back | empty |

Two more actions exist for the pedal (MIDI only): `cycle` (record, overdub,
play, overdub, play, ...) and `playStop`.

Half speed and reverse change playback only. While recording or overdubbing the
loop runs at normal speed, forwards, so what is written is what was played.
Half speed reads the loop at half the rate with linear interpolation, so it is
an octave down and twice as long. One shot ends the pass at the loop's end and
stops (position back at the start).

Mono or stereo as the chain is: the first recording fixes the channel count. A
stereo loop heard on a mono buffer is the two sides averaged; a mono loop on a
stereo buffer is on both.

**Control without locks.** Every action is an enum posted to a 32-slot ring
(`juce::AbstractFifo`). Non-audio threads post with `request()` (several
producers take turns on a spin flag that the audio thread never touches); the
audio thread posts MIDI actions with `requestFromAudioThread()` into a private
array. `process()` drains the ring and applies the actions, in order, at the
start of the block, then runs the block. The options (half speed, reverse, one
shot, quantise, level) are atomics read once per block. The audio thread
publishes state, length, position, layer counts and the recording length to
atomics at the end of each block; `looper.get` reads those, so what a command
answers is the state as of the last block, and an action it just posted shows in
`queued` until the next block applies it. Nothing in `process()` allocates or
blocks (`test/src/looper_tests.cpp` counts operator new calls).

## Commands

All in `docs/commands.md`. `looper.record`, `looper.overdub`, `looper.play`,
`looper.stop`, `looper.undo`, `looper.redo` and `looper.clear` post an action and
answer with `looper.get`'s state; `looper.setOptions` sets `halfSpeed`,
`reverse`, `oneShot`, `quantise` and `levelDb` (-60 to +12) and answers with the
state. `looper.get` reports state (`empty`, `arming`, `recording`, `playing`,
`overdubbing`, `stopped`), `memoryReady`, `closing`, `queued`, `channels`, lengths and position
in frames and seconds, `position` (0 to 1), `layers`, `canUndo`, `redoLayers`,
`canRedo`, `layersFull`, the limits and the options. Actions are `edit`
permission, like the other live controls; `looper.get` is `read`.

## MIDI

CC 48 to 61 are the looper's fixed default controls, next to the scene, tempo,
tuner and footswitch mode ones (`docs/scenes.md`). They are listed in
`midi.getMap` under `defaults`, can be moved or switched off with
`MidiMapper::setFixedControlCc`, and yield to a mapping of the user's own on the
same number. Unlike the other fixed controls they act on the audio thread, in
the block the message arrived in.

| CC | Action | Value |
|---|---|---|
| 48 | Record | a press |
| 49 | Overdub | a press |
| 50 | Play | a press |
| 51 | Stop | a press |
| 52 | Undo | a press |
| 53 | Redo | a press |
| 54 | Clear | a press |
| 55 | Half speed | 64 and above on, below off |
| 56 | Reverse | 64 and above on, below off |
| 57 | One shot | 64 and above on, below off |
| 58 | Quantise | 64 and above on, below off |
| 59 | Level | 0 to 127 over -60 to +12 dB |
| 60 | Cycle (record, overdub, play, overdub, ...) | a press |
| 61 | Play / stop | a press |

A press is 64 or above, or any value that does not follow one from the same
controller, so a momentary switch (127 then 0) is one press.

## Limits and rough edges

- Actions land at a block boundary, not on the sample; at 128 frames and 48 kHz
  that is 2.7 ms. The first recording's length is block accurate unless quantise
  is on.
- There is no compensation for the audio interface's round trip: an overdub is
  late against the loop by the output-to-input latency of the hardware.
- Quantise rounds the length of the first recording only; recording does not
  start on a beat, and the loop does not follow the tempo or the host transport
  afterwards.
- One loop for the rig. It is not part of a scene or a preset, is lost when the
  sample rate changes, and is not saved.
- Four overdubs, undone one at a time.

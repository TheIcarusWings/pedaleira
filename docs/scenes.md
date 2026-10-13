# Scenes, stomps and footswitches

The engine side of the performance controls, modelled on a floor modeller. No
UI is part of this; everything is reachable through the command layer
(`docs/commands.md`: `scene.*`, `stomp.*`, `footswitch.*`, `tempo.tap`,
`expression.*`). The code is `plugin/include/Scenes.h` (data),
`plugin/src/ProcessorScenes.cpp` (behaviour) and `MidiMapper` (the fixed MIDI
controls). Everything is keyed by block id and parameter id, never by lane or
position.

## Scenes

A rig has eight scenes, A to H. A scene holds:

1. the on/off state of every block, and
2. one value for each scene-assigned parameter.

Every other parameter has one value shared by all scenes.

**The blocks always carry the active scene.** A block's `enabled` switch, its
gains and mix and its kind's parameter values are the active scene's. Two
things follow:

- Every existing edit path is a scene edit already. `block.setEnabled` and a
  `block.setParam` of an assigned parameter write the active scene only; a
  `block.setParam` of an unassigned parameter is heard in every scene.
- The block nodes of saved state hold the active scene, so a build without
  scenes opens a rig as the scene it was saved in.

### Rules

| What | Rule |
|---|---|
| Block state | A block is in the same state in every scene until its switch is first moved. From then on each scene has its own, and a toggle writes the active one. |
| New block | Enters every scene the way it is added: on, or off everywhere for a copy of a bypassed block. |
| Removed block | Its scene data and its stomp assignments go with it (undo brings them back). A block that keeps its id but changes kind (a tone swap, a replace in place) loses the scene values of its old kind's parameters. |
| Assign | `scene.assignParam` copies the current value to all eight scenes. Assignable: `inputGain`, `outputGain`, `mix` and any parameter of the block's kind, a hosted plugin's included. |
| Not assignable | `enabled` (per scene already), `normalize`, a parameter flagged `affectsLatency` (its change mutes the chain, which a scene switch never does) and a parameter flagged for the expression pedal. |
| Unassign | The active scene's value stays as the shared one. |
| Other scenes | `scene.setParamValue` sets an assigned parameter in any scene without selecting it. |
| Copy | Block states and assigned values; the target keeps its name. Onto the active scene it is heard at once. |
| Reset | Default name, every block on, assigned values taken from the active scene. |
| Names | Default "Scene A" to "Scene H". |
| Default scene | The scene active when the rig or preset was saved. |
| Undo | Assign, unassign, values, names, copy, reset and stomp edits are undo steps. A scene switch is not. Undo restores the rig as it was when the edit was made, the active scene included. |
| Duplicate / paste | The copy starts without scene data of its own. |

### A scene switch

`scene.select` (or a footswitch, or CC 43) runs on the message thread and
makes plain stores under one short hold of the chain lock: each block's
`enabled`, the three common settings, and the kind's lock-free parameter
sets. The audio thread sees what a power click and a knob move give it:

- a block turned off fades to bypass over 25 ms, or, for a kind with
  spillover whose `spillover` switch is on, is released and rings out;
- gains and mix ride their smoothers; kind parameters are smoothed by the
  kind.

Nothing is rebuilt and the chain-edit fade (the whole-chain mute used by
structural edits) is never taken.

### Expression pedal hook

`expression.assignParam` flags a parameter as pedal-driven. The pedal does not
exist yet; the flag is stored with the rig and enforced: a flagged parameter
is taken out of the scenes and cannot be scene-assigned.

## Stomps

One assignment per footswitch A to H, saved with the rig (and in presets): one
or more blocks, an optional label and a momentary flag. A press flips each
assigned block, so one switch can turn one block off and another on. With
`momentary` the release flips them back (on while held). A stomp writes the
active scene, like any block toggle, and one press is one undo step.

## Footswitches

Eleven virtual switches: A to H, `bankUp`, `bankDown`, `tempo`.
`footswitch.press` and `footswitch.release` may come from any thread; off the
message thread they are queued without locking or allocating.

| Mode | A to H | Bank Up / Down |
|---|---|---|
| `preset` | Load preset `bank x 8 + switch` of the preset list | Move the bank by eight, wrapping |
| `scene` | Select scene A to H | Next / previous preset |
| `stomp` | Flip the switch's assigned blocks | Next / previous preset |
| `hybrid` | Per row, see below | Page whatever the rows page |

Hybrid: the top row (A to D) and the bottom row (E to H) are each `scene`,
`stomp` or `preset` (`footswitch.setHybridRows`, default scene over stomp).

- A stomp row uses its own switches' assignments.
- Scene rows: two of them show A to D on top and E to H below. A single scene
  row shows A to D, and Bank Up / Down flip it to E to H and back.
- Preset rows: four presets per row. The bank is as many presets as the rows
  show (four or eight) and Bank Up / Down move by that many.
- With nothing to page (two scene rows, two stomp rows, scene and stomp), Bank
  Up / Down step through the presets.

The preset list is the current setlist of the library, slot by slot
(`docs/library.md`): switch `n` of bank `b` is slot `b x 8 + n + 1`, an empty
slot is an empty switch, and the banks run up to the last slot in use.

The mode and the hybrid rows are machine-wide settings (keys `footswitchMode`,
`footswitchHybridTop`, `footswitchHybridBottom` in the shared
`preferences.settings` file), not part of a rig. The preset bank and the scene
page are not saved and start at the first.

`footswitch.getState` returns what a pedalboard view draws: for each switch
its role, label, colour hint (the category of a stomp's first block, else the
role), `lit`, `held` and `empty`.

`tempo` taps the tempo: the average of the last intervals (up to four), 20 to
300 BPM. A pause over 3 s starts a new count, a tap within 0.2 s of the last
is dropped, and one interval more than 30% off the average is left out (two
in a row that agree are a new tempo).

## MIDI

Fixed default controls, next to the user's MIDI map. A controller number the
user has mapped to a target of their own belongs to that mapping, and the
default on it is silent. `midi.getMap` lists them under `defaults`.

| CC | Value | Action |
|---|---|---|
| 43 | 0 to 7 | Select scene A to H |
| 44 | a press (64 or above, or any value not following one) | Tap tempo |
| 45 | 64 or above on, below off | Tuner |
| 46 | | Reserved: gig view (ignored) |
| 47 | 0 to 3 | Footswitch mode: preset, scene, stomp, hybrid |
| 48 to 61 | see `docs/looper.md` | The looper: record, overdub, play, stop, undo, redo, clear, half speed, reverse, one shot, quantise, level, cycle, play / stop |

Program changes are unchanged. `MidiMapper::setFixedControlCc` moves a default
to another controller or switches it off; that is not saved or exposed as a
command yet.

## Saved shape

Two optional children of `ChainSnapshot`, after the lanes, in plugin state,
presets and undo snapshots alike. Each is written only once it holds
something, so a rig that never used scenes or stomps saves as it always did.

```
Scenes version=1 active=<0..7>
  Scene index=<0..7> [name="..."]              (eight of them)
    Block id="<block id>" enabled=<bool>       (blocks whose switch has been moved)
    Value blockId="<block id>" param="<param id>" value=<double>
  Expression blockId="<block id>" param="<param id>"
Stomps version=1
  Stomp switch=<0..7> [label="..."] momentary=<bool>
    Block id="<block id>"
```

A parameter is scene-assigned when the scenes hold a `Value` for it. A rig
without a `Scenes` node loads with every scene equal to the saved state and
scene A active.

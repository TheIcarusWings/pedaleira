# Importing presets from other products

Phase 5. The importer is generic: one small reader per source writes a neutral
"foreign preset" file, and the engine turns that file into a Pedaleira grid
preset with the help of a mapping table (one per source) and tells the player
what it carried over and what it could not. The first reader is for the Quad
Cortex. The Import page of the app reads the dump
files the reader writes and shows the report this document describes.

```
 your unit --USB--> reader (Python)  --> foreign preset .json --+
                    tools/import/                                |
                                                                 v
        mapping table (JSON, one per source) --> PresetImporter --> grid preset
        resources/import/quad-cortex.json         (engine)           + report
```

Everything goes through the command layer (`preset.importForeign`,
`docs/commands.md`) and through `PresetTool --import-foreign` for a dry run on
the command line.

## What this does and does not do

- It reads **your own** presets from **your own** unit, with the data the unit
  already hands to any USB client: block names, parameter values, scenes and
  routing. It then maps them onto **our** blocks.
- Nothing from Neural DSP's code, firmware or models is in this repository.
  Nothing is decompiled or decrypted, and no Neural Capture is read or
  converted. The reader never writes to the unit.
- An **amp, a cab or a capture of the source is never mapped** to a model of
  ours. Its slot stays empty and the report says to pick a NAM capture or an
  IR. (TONE3000's own policy also forbids sharing captures of current
  modellers.)
- A model the table does not know is never guessed. It falls back to a plain
  block of its category at default settings (reported as approximated) or
  stays empty (reported as unmapped).

## The foreign preset format

`"format": "pedaleira-foreign-preset"`, `"version": 1`. UTF-8 JSON. A file with
another format, no version or a higher version is refused with the reason.
Rows and positions are **zero based**. Unknown members are ignored.

```json
{
  "format": "pedaleira-foreign-preset",
  "version": 1,
  "source": "quad-cortex",
  "sourceVersion": "CorOS 4.0.1",
  "name": "Clean With Verb",
  "tempo": 100,
  "defaultScene": 0,
  "rows": [
    { "index": 0, "blocks": [
      { "position": 0, "model": "VCA Comp (M)", "category": "Compressor", "enabled": true,
        "params": { "THRESHOLD": { "value": -20.0, "unit": "dB" } } },
      { "position": 4, "model": "Digital Delay (ST)", "category": "Delay", "enabled": true,
        "params": { "FEEDBACK": { "value": 30.0, "unit": "%" }, "TYPE": { "value": "PING PONG" } } }
    ] }
  ],
  "routing": {
    "rows": [
      { "index": 0,
        "input":  { "type": "device", "channel": 1, "stereo": false, "name": "INPUT_1" },
        "output": { "type": "device", "pair": 1, "name": "XLR_1_2" } }
    ],
    "splits": [ { "row": 0, "splitAt": 2, "mixAt": 6 } ]
  },
  "scenes": [
    { "name": "Clean", "enabled": { "0:4": false }, "params": {} },
    { "name": "Wet", "enabled": { "0:4": true }, "params": { "0:4": { "FEEDBACK": 55.0 } } }
  ],
  "warnings": []
}
```

| Field | Type | Meaning |
|---|---|---|
| `format`, `version` | string, int | Always `"pedaleira-foreign-preset"` and `1`. |
| `source` | string | Which product and reader: `"quad-cortex"`. Picks the built-in mapping table, and has to match the `source` of a table given by hand. |
| `sourceVersion` | string | The firmware or tool version the data came from, for the player's information. May be empty. |
| `name` | string | The preset's name. Blank becomes "Imported preset". |
| `tempo` | number | Optional, BPM. Becomes the preset's tempo. |
| `defaultScene` | int 0 to 7 | Optional, default 0. The scene the preset was saved in: the one it starts in, and the one the block values (`enabled`, `params`) belong to. |
| `rows` | array | Only rows that hold blocks need to be listed. |
| `rows[].index` | int 0 to 3 | The row. A row outside the grid is reported and its blocks are left out. |
| `rows[].blocks[].position` | int 0 to 7 | The slot in the row (the source's column). A position that does not fit, or one already taken, is reported as unmapped. |
| `...model` | string | The source's own name for the block ("VCA Comp (M)"). Matched against the table without regard to case or punctuation, and a trailing `(M)`, `(ST)` or `(S/C)` is ignored. |
| `...category` | string | The source's category ("Compressor", "Guitar Amplifier"). Decides the fallback when the model is not in the table. |
| `...enabled` | bool | Not bypassed. Default true. |
| `...params` | object | By the source's parameter name. Each is `{ "value": ..., "unit": ... }` (a bare value is accepted). `value` is a number, a bool or a string (the name of a list option, or text such as a file name). `unit` is the source's own unit string and is optional. Values are in the units the source's screen shows, not its internal scale. |
| `routing.rows[]` | array | Optional. `index`; `input`: `{ type: "none" \| "device" \| "other", channel (1 based), stereo (bool), name }`; `output`: `{ type: "none" \| "device" \| "row" \| "other", pair (1 based), name }`. `none` is a row fed by a split or another row. `device` is one of the unit's main jacks. `row` feeds the row below. `other` is a port with no counterpart here (return jacks, USB, a multi-output): it is named in the report. Without any routing, row 0 gets input 1 and output 1/2. |
| `routing.splits[]` | array | `row` (the upper row; the lane lives in the row below), `splitAt` (the slot boundary where the lane leaves, 0 to 7, 0 being before the first slot) and `mixAt` (where it rejoins, after `splitAt` and up to 8, or null for a lane that never rejoins). |
| `scenes[]` | array | Up to eight, A to H. `name`; `enabled`: `"<row>:<position>": bool` for blocks whose on/off state follows scenes (given for every scene); `params`: `"<row>:<position>": { "<param>": value }` for parameters whose value in this scene differs from the one in `defaultScene`. A key that is absent means "as the block has it". Names are in the source's parameter names and units, and go through the same table. |
| `warnings` | array of strings | Things the reader could not read. Informational. |

## The Quad Cortex reader

`tools/import/quadcortex_dump.py`, with `tools/import/README.md` for setup and
`tools/import/test_quadcortex_dump.py` for its tests (no unit or library
needed). It uses [pyquadcortex](https://github.com/stokes-audio/pyquadcortex)
(MIT, unofficial), which talks to the unit over USB with the protocol the unit
already speaks. Install it, quit Cortex Control (it holds the USB interface),
connect the unit and run:

```
python3 tools/import/quadcortex_dump.py --current --out dumps/            # what is on the grid
python3 tools/import/quadcortex_dump.py --slot 28C --out dumps/           # one preset by slot
python3 tools/import/quadcortex_dump.py --preset "Brit 2203" --setlist factory --out dumps/
python3 tools/import/quadcortex_dump.py --all --setlist user --yes --out dumps/
```

**Read only.** The script contains no call that sets, saves, deletes or moves
anything. One thing is not ours to fix: the unit offers no way to read a stored
preset without loading it, so `--slot`, `--preset` and `--all` **recall** each
preset on the unit (the audio cuts for a moment and unsaved edits on the unit
are discarded). `--current` reads the grid as it is and recalls nothing; `--all`
needs `--yes` for that reason.

How pyquadcortex's objects become the fields above (this is
`build_foreign_preset`, a pure function):

| Foreign field | pyquadcortex |
|---|---|
| `name`, `defaultScene`, scene names | `preset.name`, `preset.default_scene`, `preset.scene_labels` (blank labels become "Scene A" and so on) |
| `rows[].blocks[]` | `protocol.blocks(preset)`: only occupied cells (every row reports eight) |
| `model`, `category` | the catalog entry for the block's model id: `qc.catalog[id].name` and `.category` |
| `params` | each visible catalog parameter of the model, read with `protocol.param_state` and converted from the unit's 0 to 1 line to the screen's units with the catalog's range and taper (`Parameter.to_real`); a list parameter becomes the option's name; a string parameter (a capture's file name) stays text. A value that cannot be converted (NaN in a factory preset, an unmeasured range) is left out |
| `enabled`, `scenes[].enabled` | `protocol.bypass_state`: global, or one flag per scene when `scene_mode` is on |
| `scenes[].params` | `protocol.param_state` values where `scene_mode` is on and the scene's value differs from the default scene's |
| `routing.rows` | `chain.in_portid` and `chain.out_portid`, as `Input` and `Output` names |
| `routing.splits` | `protocol.splits(preset)`: `split_column` is `splitAt`, `mix_column` is `mixAt` (null when it is -1) |
| `tempo` | the preset's `TEMPO` parameter in BPM, when readable |
| `sourceVersion` | `qc.version().app_fw_version` |

pyquadcortex says it is verified on CorOS 4.0.1; a CorOS update may break it.
Mixer and splitter types and levels, lane volume and pan, footswitch
assignments, expression pedals and MIDI are not read yet.

## The mapping table

`resources/import/<source>.json`, built into the app (`PedaleiraImportData`,
see `cmake/ImportData.cmake`); `preset.importForeign` takes another one with
`mapping`. Names are compared without regard to case, spaces and punctuation.

```json
{
  "source": "quad-cortex",
  "version": 1,
  "models": {
    "Digital Delay": {
      "kind": "delay.digital",
      "verified": false,
      "params": {
        "FEEDBACK": { "param": "feedback", "unit": "%", "transform": "linear" },
        "MIX": { "param": "mix", "unit": "%", "transform": { "scale": 0.01, "offset": 0 } },
        "TYPE": { "param": "sync", "transform": { "map": { "Free": 0, "Synced": 1 } } }
      },
      "defaults": { "toneHz": 5000 },
      "notes": "Shown in the report."
    },
    "Brit 2203": { "role": "amp" }
  },
  "categories": {
    "Delay": "delay.digital",
    "Filter": null,
    "Guitar Amplifier": { "role": "amp" }
  }
}
```

| Field | Meaning |
|---|---|
| `source`, `version` | The source id (has to match the foreign file's) and the table format, 1. |
| `models` | By the source's model name; a trailing `(M)`, `(ST)` or `(S/C)` on either side is ignored, so one entry covers the mono and stereo versions. |
| `models.*.kind` | Our kind id (`kinds.list`): `delay.digital`, `reverb.hall`... A kind this build does not have makes the block unmapped, not an error. |
| `models.*.verified` | `true` only when someone has checked this entry against a dump from a real unit. Shown in the report. Every entry of the built-in table is `false` today. |
| `models.*.params` | By the source's parameter name. `param` is one of our parameter ids for that kind, or a common setting: `mix`, `inputGain`, `outputGain` (knob positions, 0 to 1) or `enabled`. `unit` (optional) is the unit the entry was written for: a dump that says another unit is not converted by it, it is reported. |
| `...transform` | `"linear"` (the number as it is), `{ "scale": a, "offset": b }` (`value x a + b`), `{ "scale": a, "offset": b, "exponent": e, "inputMin": n, "inputMax": m }` (a curve: `a x ((value - n) / (m - n)) ^ e + b`, `n` defaulting to 0 and `m` to 100), `{ "table": { "inputMin": n, "inputMax": m, "points": [[position, value], ...] } }` (the knob's position 0 to 1 on its range `n` to `m` looked up in the points, joined in the logarithm of the value), or `{ "map": { "<their value>": <our value> } }` (a list option, bool or number to our number; the key is compared like a name). A string that is the label of one of a choice parameter's options becomes its index. |
| `models.*.defaults` | Our parameter ids and the values they start at, before the mapped ones. |
| `models.*.role` | `"amp"`, `"cab"` or `"capture"` instead of a `kind`: never mapped, the report says to pick a capture or IR. |
| `models.*.notes` | A sentence for the report's `mapped` entry. |
| `categories` | By the source's category. A kind id is the fallback for a model the table does not name (placed at its default settings, reported as approximated); `null` means no fallback (unmapped); `{ "role": ... }` as for a model. |

Values out of our range are clamped and reported; a source parameter with no
entry is reported in `droppedParams`.

## What the importer does

`PresetImporter` (`plugin/src/PresetImporter.cpp`) takes the two parsed JSON
documents and returns the preset as text (the canonical preset JSON of
`docs/library.md`, written by the library's own code, so `preset.importJson`
reads it and the same input gives the same bytes), the preset as a value and
the report. Block ids are made from the source, the preset name and the
position, so they are stable.

- **Layout.** The preset is a grid preset (`layout: "grid"`, four rows of eight
  slots). A block sits at its row and position; every
  other slot is an empty slot. A row is a stereo row; a single input jack is a
  mono input into it. Row inputs and outputs come from `routing`.
- **Splits.** `splitAt` and `mixAt` are taken as our boundaries. A split is
  kept when the grid rules allow it: a row splits into the row directly below,
  one split per row, `splitAt` before `mixAt`, a split nested in another inside
  its range. The row below then takes no input of its own (its input is
  dropped and noted) and, if the lane rejoins, no output. A split that does not
  fit is flattened and a note says so. A row that feeds the row below
  (`output.type: "row"`) is kept when that row can take it, else it goes to
  outputs 1/2. Both the split type and the mixer levels are the defaults (an
  A/B split at unity); the source's are not read yet.
- **Scenes.** Our scenes hold a block's on/off state and one value per
  scene-assigned parameter (`docs/scenes.md`). Both are carried for mapped and
  approximated blocks; a parameter becomes scene-assigned when its value
  differs between scenes. Not carried, and reported: states and values of
  blocks that were not mapped, parameters the table does not know, and
  parameters that cannot follow scenes (a switch, or one that changes
  latency). Scene names are kept unless they are the default. The block values
  in the saved slots are those of `defaultScene`.
- **Not carried at all** (the model has no place for it): scene-specific
  tempo, scene colours, footswitch assignments, expression pedals, MIDI
  messages, the mixer and splitter settings, lane volume and pan.

### The report

`{ mapped, approximated, unmapped, droppedParams, notes }`; `preset.previewForeign` returns the same report without saving (the dry run). Every entry names
the block by `row` and `position` (zero based) and the source's `model`.

| List | Entry |
|---|---|
| `mapped` | `kind`, `verified`, `notes?`: a block the table names, placed with its parameters. |
| `approximated` | `type: "block"` with `category`, `kind`, `reason`: a fallback placed at defaults. `type: "param"` with `param`, `ourParam`, `from`, `to`, `reason`: a value that was out of range and clamped. |
| `unmapped` | `category`, `cause`, `reason`, `role?` (`amp`, `cab`, `capture`): the slot is empty. `cause` is a short code for screens: `role` (an amp, cab or capture), `notInTable`, `noFallback`, `mapsToNothing`, `unknownKind` (a kind this build lacks), `position` (outside the grid) or `occupied`. The reason for a role says to pick a NAM capture or an IR. |
| `droppedParams` | `param`, `value`, `reason`, `scene?`: a source parameter that was not carried. |
| `notes` | Sentences: ports and splits that were changed, scene totals. |

## Running an import end to end

1. Dump from the unit (see above). One JSON file per preset.
2. Look first, without touching the library:
   `build/tools/PresetTool_artefacts/Release/PresetTool --import-foreign dumps/28C\ Brit.json > /tmp/preset.json`
   prints our preset JSON on stdout and the report on stderr. `--mapping
   <file>` tries a table of your own. (Build with `cmake --build build --target
   PresetTool`.)
3. Import: `preset.importForeign { "file": "/path/dumps/28C Brit.json" }`,
   optionally with `slot`, `setlist` and `mapping`. Permission `edit`; it goes
   into an empty slot like `preset.importJson`, loads nothing and returns the
   new preset and the report. A client of the command server may only name
   files inside the allowed folders (`docs/agents.md`).
4. Load the preset, add the NAM captures and IRs the report says are missing,
   and listen.

## The built-in Quad Cortex table: sources

Only public facts went in, and only where they were reliable. The table is
deliberately short: where a parameter's name or unit could not be found, the
parameter is left out (it is then reported as dropped) rather than guessed.

- Neural DSP, *Quad Cortex User Manual* (online: <https://neuraldsp.com/manual/quad-cortex>,
  PDFs for CorOS 1.x to 4.x on downloads.neuraldsp.com): the block categories
  (compressor, delay with digital, analog and tape devices, EQ, filter, wah,
  pitch, reverb, modulation), the **Trails** switch, the reverb **Pre Delay**
  (delays the first reflection by up to 100 ms), the reverb high and low pass
  controls, and the general **Rate**, **Depth** and **Sync** controls of the
  modulation blocks.
- Neural DSP support, *common FX parameters*
  (<https://support.neuraldsp.com/help/common-fx-parameters>): the threshold
  of a compressor, the feedback / mix of delays and reverbs, decay, pre delay,
  high and low pass.
- Neural DSP, *Quad Cortex device list* (<https://neuraldsp.com/device-list>):
  the names of the reverbs (Hall, Plate, Plate Lush, Plate Tight, Room,
  Shimmer, Spring) and modulation devices (Chorus Engine, Digital Flanger,
  Flanger Engine, Flangerish, Phaser, Rotary, Tremolo, Vibrato, Vintage
  Chorus).
- pyquadcortex's own documentation and its list of factory block names grouped
  by category (<https://github.com/stokes-audio/pyquadcortex>, MIT, CorOS 4.0.1
  snapshot): the category names (Compressor, Delay, Equalizer, Filter,
  Modulation, Pitch, Reverb, Wah, Utility, Guitar Amplifier, Bass Amplifier,
  Cabsim..., Guitar Overdrive, Bass Overdrive, Neural Capture, ...), the model
  names of compressors, delays, gates, pitch and wah blocks, and the one
  parameter it documents by name and unit for a compressor: `THRESHOLD`, in dB.
  Only names were taken from it, no data files.

How these were read: the sandbox this table was written in could not open
neuraldsp.com pages directly, so the manual, support and device-list facts come
from search results that quote them, not from a full read of the pages. That is
one more reason every entry is `verified: false`, and why the units in the
table are checked against what a dump says (`unit`).

What is in it: compressors (VCA Comp's threshold; the other compressors fall
back to a compressor at defaults), gates, a graphic EQ, five delays (feedback
and mix), seven reverbs (pre delay and mix), the chorus, flanger, phaser,
tremolo and vibrato blocks (rate and depth), envelope filter, auto wah, wahs,
pitch shifter and octaver, and the factory drive models (overdrives,
distortions, fuzzes, boosts and the bass drives) onto the `drive.*` kinds, and the utility blocks Gain,
Volume, Doubler and Phase Doctor onto `utility.*` (parameter names and units
assumed, not read from a dump; Phase Doctor carries only its polarity switch).
The drive entries come from the models' names and the usual controls of their
kind, not from a dump: their knobs are assumed to read 0 to 10 and to carry
the names the entries list (a name that does not match is reported as a
dropped parameter), so each one is a starting point to set by ear. The
`Guitar Overdrive` category falls back to `drive.overdrive` and `Bass
Overdrive` to `drive.bass`. Categories we have no block for (fixed filters, synths, loopers) are unmapped, and amps, cabs, IR loaders and
captures are never mapped.

## The equaliser blocks: measured laws

The four equalisers carry laws measured on a unit, not a straight line from the
screen's range. The dump gives each knob on its linear screen range (the wire
position 0 to 1 spread over the range); the table undoes that and applies the
unit's real law. Wire below means that position.

- **Graphic 9.** Band gains are the dB shown, 1:1. The 65 Hz band is a low
  shelf, 125 Hz to 8 kHz are peaking at those centres, 16 kHz is a high shelf;
  the Q control (0 to 10, value = Q, default 1) is the Q of every band. The
  high-pass (19 to 500 Hz) and low-pass (1000 to 20001 Hz) are second order
  with Q 1.0 and their corner is `lo + span x n ^ (5/3)`. It lands on the
  graphic EQ block with its **Band style** set to Shelved ends (the 63 Hz band
  becomes the 65 Hz low shelf, the 16 kHz band a high shelf, **Band Q** sets
  every band's Q), and its high-pass and low-pass switched on. Our 31.5 Hz band
  stays flat. In the default Octave style the block is exactly what it was.
- **Parametric-3 and Parametric-8**, every band the same: gain is
  `-12 + 24 x wire` dB (0.5 is flat), Q is `0.1 + 9.9 x wire`, frequency is
  `20 + 19980 x wire ^ (10/3)` Hz, type is peaking, high-pass, low-pass, high
  shelf, low shelf for wire 0, .25, .5, .75, 1 (the pass types second order),
  and BYPASS 0 is a band that is off. Every band can be any of the five, so
  each band of our blocks has a **Shape** control (Default keeps the older
  Peak/Shelf type control, so existing presets are unchanged) and the import
  sets it. The blocks are given the unit's loaded state first: the
  Parametric-3 is low shelf 125 Hz, peak 1000 Hz, high shelf 4000 Hz; the
  Parametric-8 has a low shelf at 50 Hz, peaks at 100 Hz and 3 kHz, a high
  shelf at 1 kHz, all Q 0.71, bands 5 and 8 bypassed, and band 8 is a
  second-order low-pass with a fixed Q of 0.707 (its Q control does nothing,
  so it is not read).
- **Parametric-4** (our Console EQ). Gains are the dB shown. All four bands
  are peaking with a fixed Q: LO 0.46, LO MID 0.50, HI MID 0.54, HI 0.58; the
  import sets the Console EQ's **Band style** to Peaking, which holds those
  Qs (the default Console style keeps the shelves). Frequency follows a table
  of the knob position (0, .1, .25, .5, .75, .9, 1), joined in the logarithm
  of the frequency: LO 20.1, 21.6, 28, 60, 100, 160, 200 Hz; LO MID 100, 108,
  140, 260, 500, 680, 1000; HI MID 500, 540, 700, 1200, 3000, 4200, 5000;
  HI 2000, 2160, 2800, 6000, 12000, 16818, 20000. The high-pass (20 to 500 Hz)
  and low-pass (1000 to 20000 Hz) are Butterworth and linear in the wire.

What is assumed, not measured: that the screen ranges are the ones above (a
dump with other ranges needs the table's `inputMin` and `inputMax` changed),
and the names of the type and bypass options (the numbers 0 to 4 and the usual
names are read).

## What still needs a real unit

- A dump from João's unit, to check the table against real model names,
  category spellings, parameter names and units. Until then every entry is
  `verified: false`. First things to learn from it: delay time and the other
  delay controls, reverb decay and filters, the modulation controls of each
  block, the compressors' other parameters, whether `MIX` is a percent.
- Whether `splitColumn` and `mixColumn` in pyquadcortex are slot boundaries or
  slot numbers (the importer treats them as boundaries and says so in a note).
- Mixer and splitter settings, lane volume and pan, and the type of each split
  (the reader does not read them yet).
- Setting `verified` to `true` entry by entry as they are checked.

## Known limits

- One reader exists. A second source is a reader that writes the foreign
  format and a table in `resources/import`; the engine needs no change.
- Block ids are derived from the preset name and positions: importing the same
  preset twice gives two presets with the same block ids, which is harmless
  (ids only have to be unique within a rig).
- A row that holds more than eight blocks cannot exist in the source (eight
  columns), but two blocks given for one position are reported, not merged.

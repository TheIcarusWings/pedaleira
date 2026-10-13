# Matching sessions with a Quad Cortex

`qc_match.py` drives **your own** Quad Cortex (QC) over USB, with
[pyquadcortex](https://github.com/stokes-audio/pyquadcortex) (MIT, an unofficial library that speaks
the protocol the unit already speaks), and runs the measurement of `docs/matching.md` for you: for
each take of a plan it sets up the QC, records its output with `PedaleiraMatch record`, renders our
equivalent effect with `PedaleiraRender` over the same test signal, analyses and compares both with
`PedaleiraMatch`, and writes a Markdown report. Nothing of the unit is decoded, decrypted or copied;
only what comes out of its output is measured.

## The never-save rule

**This script never stores anything on the unit.** No preset is saved, no setlist or setting is
written, no global is changed (no I/O or USB levels, no global EQ, no MIDI, no tuner, no mode).

- In code: the unit is only reached through a small allow-list proxy (`GuardedClient`). It exposes
  the reads (`version`, `catalog`, `find_preset`, `list_presets`, `read_preset`, `read_current_preset`,
  `preset_dirty`), the loads (`recall_preset`, `read_preset`) and the grid edits one take needs
  (`set_block`, `remove_block`, `set_chain_input`, `set_chain_output`, `set_param`, `set_bypass`,
  `switch_scene`), plus `close`. Anything else, `save_current_preset` and `write_preset` first of
  all, raises an error before it reaches the unit. The tests check this call by call.
- Before the grid is touched the script asks the unit whether the preset on it has unsaved edits. If
  it has, the script stops and tells you to save or discard them on the unit. Your work is never
  overwritten. The one exception is the scratch preset itself (see "Leftover edits on the scratch
  preset" below).
- It then tells you which preset is recalled as the scratch preset (the audio cuts for a moment),
  and works on one **free row** of it only. Every change is an unsaved edit of the grid.
- When it ends, it recalls another preset of the same setlist and then the scratch preset again (the
  unit does not reload a preset that is already loaded), which discards its own edits (the unit had none
  of yours when it started). The audio cuts twice more. Then it says that nothing was saved. **Recalling any preset on the unit restores it.**

## Set up

1. Python 3.11 or newer, standard library only for the script itself.
2. `pip install pyquadcortex`, and the hidapi library: macOS `brew install hidapi` (run with
   `DYLD_LIBRARY_PATH=/opt/homebrew/lib` in front of the command), Debian and Ubuntu
   `sudo apt install libhidapi-hidraw0`, Windows: included.
3. **Quit Cortex Control.** It holds the USB interface exclusively; the script cannot open the unit
   while it runs and says so.
4. Connect the unit by USB and switch it on. Work at **48 kHz** (the test signal and the recorder
   are 48 kHz; set the same rate in your operating system's audio settings for the unit).
5. Build the two tools: `cmake --build build --target PedaleiraMatch PedaleiraRender`. The default
   paths are `build/tools/PedaleiraMatch_artefacts/Release/PedaleiraMatch` and
   `build/tools/PedaleiraRender_artefacts/Release/PedaleiraRender`; `--match-tool` and
   `--render-tool` give others. `PedaleiraMatch record` is the recorder command; if your build has
   no `record` command yet, the session reports it on the first take.

### USB routing

The session routes **one free row** of the scratch preset: input `USB_5_6`, one block in column 1,
output `USB_3_4` (pyquadcortex's `Input.USB_5_6` and `Output.USB_3_4`). Seen from the computer,
that is: play the test signal on the QC's USB output channels **5 and 6** (`--out-ch 5,6`) and
record its USB input channels **3 and 4** (`--in-ch 3,4`). These channel numbers are what the
port names suggest and have **not been checked on a unit**. If your operating system shows the QC
with a different channel layout, change them: `--qc-input`, `--qc-output` (any pyquadcortex port
name, for instance `USB_7_8`), `--out-ch`, `--in-ch`, `--device` (the name of the audio device).

What the script does not touch, because they are settings and not grid edits: the USB input and
output levels, the row's lane volume and its input gate. Leave them at unity and off, and check the
`bypass` take (the unit's own path) against `identity` first.

### A scratch preset of your own

By default the session borrows a free row of the factory preset `Brit 2203`, which every unit has.
An empty preset is cleaner: nothing else runs beside the take, and no preset you play is touched.
The script cannot make one (that would be a save), so make it once yourself: a new empty preset on
the unit, named for instance `Pedaleira Scratch`, saved to a free slot of My Presets. Then add
`--scratch-preset "Pedaleira Scratch" --scratch-setlist user` to every run. The script only recalls
it and edits one row without saving, and the end of the session reloads it empty.

### Leftover edits on the scratch preset

If the preset loaded on the unit **is the scratch preset** (the name given by `--scratch-preset`, or the
preset at `--scratch-slot`, in `--scratch-setlist`) and it has unsaved edits, they can only be leftovers
of an earlier session: the scratch preset holds nothing of value. The script then clears them instead of
stopping. Recalling the preset that is already loaded does not reload it on the unit, so it recalls the
first factory preset that is not the scratch (read from the factory setlist), then the scratch again,
re-checks that the unsaved-edit flag is gone and that the routing of the four rows is the one the scratch
preset stores. If the flag is still set, or the routing differs, it stops as before (exit 4). Nothing is
saved. Every recall (these, the scratch recall for the session and the two at the end) is printed and
listed in `report.md` under "Recalls made on the unit".

A preset other than the scratch with unsaved edits still stops the session, and so does `--use-current`
(it never recalls anything). The preset on the grid is recognised by its name; the unit does not say
which setlist it came from, so a user preset that has the same name as the scratch preset counts as the
scratch.

### Curved parameters

pyquadcortex converts every parameter linearly between its catalogue minimum and maximum, but the
unit plays some on a power curve: `min + (max - min) * wire ^ exponent`. A real unit (CorOS d14e)
played Digital Delay's DELAY TIME 250 ms as 35 ms and VCA Comp's RATIO 4 as 2.8 until the values were
sent on the curve. `resources/import/quad-cortex-curves.json` lists the parameters measured so far;
this script and the dump script apply them. A parameter not in it is sent linearly, so a new one is
worth one timing take before its results are trusted.

Not every difference is a curve. VCA Comp compresses from 1.28 dB below the THRESHOLD it shows, at
-20 and at -30 alike, so the import table (`quad-cortex.json`) takes that off as an offset instead.

## Run

```
# Read the unit's block catalogue (read only) and write it down.
python3 tools/match/qc_match.py --catalog --out qc-reports

# Compare that catalogue with resources/import/quad-cortex.json (read only; the file is not changed).
python3 tools/match/qc_match.py --check-mapping --out qc-reports
python3 tools/match/qc_match.py --check-mapping --catalog-file qc-reports/qc-catalog.json --out qc-reports

# A dry run: prints every grid edit, recording and render. Does not contact the unit.
python3 tools/match/qc_match.py --plan tools/match/example-plan.json

# The real session.
python3 tools/match/qc_match.py --plan tools/match/example-plan.json --go --dir match
```

Without `--go` a session is always a dry run. `--dry-run` says so explicitly; `--go` and
`--dry-run` cannot be combined.

| Option | What it does |
|---|---|
| `--plan FILE` | The takes (below). |
| `--go` | Run the session on the unit. |
| `--dir DIR` | Where the session goes (default `match`): `signal/`, `ref/`, `ours/`, `analysis/`, `presets/` and `report.md`. |
| `--scratch-preset NAME` | The preset recalled as scratch (default `Brit 2203`, a factory preset). The script stops if it has no free row; pick another. |
| `--scratch-slot SLOT`, `--scratch-setlist factory\|user` | Choose the scratch preset by slot instead, or from My Presets. |
| `--use-current` | Work on the preset on the grid now and recall nothing. It still has to be free of unsaved edits. |
| `--row 1..4` | The row to use, as the unit numbers it (default: the first free row). |
| `--qc-input`, `--qc-output`, `--out-ch`, `--in-ch`, `--device` | The routing (above). |
| `--type`, `--rate`, `--buffer` | Passed on to `PedaleiraMatch record` (`--rate` default 48000). |
| `--settle S` | Seconds to wait after the grid edits (default 1.5). |
| `--tail S`, `--render-out-channels N` | Passed on to `PedaleiraRender` (`--tail` default 4). |
| `--yes` | Do not ask before the scratch preset is recalled. |
| `--overwrite` | Replace a recording that exists already. The default is to fail the take: a recording is a measurement. |
| `--no-readback` | Do not read the block's parameters back from the unit (see "Parameter readback"). The default is to read them. |
| `--mapping FILE` | A mapping table other than `resources/import/quad-cortex.json`. |

A failed take check of the recorder (a non-zero exit) is tried once more; a take that still fails is
listed in the report and the session goes on with the next one.

## The plan

A JSON file with a list of takes (`example-plan.json` has a delay at three times, a compressor at two
ratios, a reverb at two decays and a bypass take):

```json
{
  "format": "pedaleira-match-plan",
  "version": 1,
  "takes": [
    { "name": "delay-digital-250ms-fb35", "model": "Digital Delay",
      "params": { "TIME": 250, "FEEDBACK": 35, "MIX": 100 },
      "ours": { "params": { "timeMs": 250 } } },
    { "name": "bypass", "bypass": true }
  ]
}
```

| Field | Meaning |
|---|---|
| `name` | The file stem of the take (letters, digits, `.`, `_`, `-`). Unique. |
| `model` | The QC block, by catalogue name or by id (needed when a name is ambiguous, for instance a mono and a stereo version). Names ignore case, spaces and punctuation. |
| `params` | QC parameters by their catalogue name, **in the parameter's own units** (ms, dB, %, ...). A list parameter takes its option index. A parameter whose range the unit publishes only as 0 to 1 takes `{ "wire": 0.5 }`. Parameters you leave out stay at the block's defaults, because every take starts from a fresh block. |
| `ours` | Optional. `kind` (a kind id such as `delay.digital`), `params` (our parameter ids and values), and the common settings `mix`, `inputGain`, `outputGain`, `enabled`. What it gives wins over the mapping. |
| `signalGainDb` | Optional, a number from -60 to 0 (default 0): play the test signal that many dB below nominal for this take (see "Signal level"). |
| `bypass` | `true` for the unit's own path: no block on the row; ours renders an empty chain. |
| `notes` | Free text for the report. |

Our side is derived from `resources/import/quad-cortex.json` (the QC model gives our kind, a mapped
parameter gives our parameter through its transform, `MIX` in percent becomes our mix 0 to 1) unless
the plan says otherwise. The report says, for every take, whether the mapping is **verified** (every
entry in the file is `verified: false` today, so it never is yet), and names each QC parameter that
has no mapping and is therefore set on the unit only. The delay time, for instance, is not mapped,
which is why the example gives `timeMs` itself. Keep the mix at 100% wet on the unit (and ours is then
1.0) for the cleanest delay and decay figures; say so in the take name.

The recording is stereo when `--in-ch` names two channels, and ours is rendered with as many
channels as are recorded, so both sides are compared alike. `--render-out-channels` overrides it.

A bypass take on a real unit (CorOS d14e, over USB) matches our empty chain except for one thing:
our output's DC blocker (a 5 Hz high-pass) leaves a short tail after the burst (T60 about 0.22 s,
over in 180 ms) and moves the lowest sweep bands by a quarter of a dB. Expect those two burst rows
to fail on every take until that is decided.

### Signal level

On a delay or modulation take with MIX 50 the unit adds the wet signal to the full-level dry one, and
the test signal's 0 dBFS step can clip the recording, so the take fails its check. Lower the signal with
`signalGainDb`, per take or as a default for the whole plan (a take's own value wins, `0` included):

```json
{
  "format": "pedaleira-match-plan",
  "version": 1,
  "signalGainDb": -6,
  "takes": [
    { "name": "delay-mix50", "model": "Digital Delay (M)", "params": { "DELAY TIME": 250, "MIX": 50 } },
    { "name": "bypass", "bypass": true, "signalGainDb": 0 }
  ]
}
```

How it works. `PedaleiraMatch record --gain-db G` plays the signal scaled by G dB (a build that has
it is needed for a non-zero gain; at 0 the option is not passed). For ours the script writes a scaled
copy of the signal WAV into `<dir>/signal/` once per gain value (`pedaleira-test-signal-v1-atten-6dB.wav`)
and renders ours from it, so both sides see the same levels and the comparison stays like for like.

The clip check is unchanged: it looks at the recording itself, so a take that still clips fails as
before (the report's failed-takes table then hints at `signalGainDb`). The recorder's level at 1 kHz is
printed against what was played. The analysis (`analyse`, `compare`) measures against the nominal
signal and is not told the gain: on both sides the sweep level gain reads G dB lower than the block's
true gain, and the input level of each static-curve step is its nominal level plus G (the outputs are
the true levels). Both sides carry the same offset, and the report says so for every take that uses a gain, with
the recorder's peak, problem and verdict lines ("Recording check") for every take.

### Parameter readback

Some parameters may not be linear between the wire value the script sends and the value the unit shows
and plays (Tremolo RATE 2, 5 and 8 played at 0.49, 2.43 and 4.37 Hz; Simple Delay DELAY TIME 300 played
500 ms). After the grid edits and before the recording the script therefore reads back what the unit
holds, and `report.md` has per take a table: the parameter, the value asked, the wire value sent, the
wire value the unit reports, that value converted to the parameter's units (linearly, as pyquadcortex
does, and on the measured curve of `quad-cortex-curves.json` where there is one) and the difference
between the converted value and the one asked. A line of the note column says when the unit holds a
different wire value from the one sent.

pyquadcortex has no call that reads one parameter. The script reads the live grid with
`read_current_preset()` (the call that checks the block and the routing landed, so it costs no extra
read: no side effects, unsaved edits and the active scene untouched) and takes each parameter from it
with `pyquadcortex.param_state(preset, row, column, index)`, which only decodes the preset in memory.
The first value slot is used (a fresh block does not follow scenes). Nothing is written to the unit and a
take is not slowed down. `--no-readback` skips the table.

## The report

`<dir>/report.md`: the session (firmware, scratch preset, row, ports, every recall made on the
unit), a summary table, then for each take the QC settings, our settings and where each came from, the
mapping status, the signal level statement and the parameter readback table (above), the recorder's
check lines, the overall
score, the score per metric group (the groups of `docs/matching.md`), the
metrics out of tolerance, and the files. Takes that failed are listed at the end with the stage that
failed. The full per-take comparison tables are `<dir>/analysis/<take>.md`.

## Catalogue and mapping check

`--catalog` writes `qc-reports/qc-catalog.json`: every model of the unit with its category, ids and
for every parameter its name, range, default, unit and type. Read only; it needs no scratch preset.

`--check-mapping` compares that with `resources/import/quad-cortex.json` and writes
`mapping-check.md` and `mapping-check.json` (it reads the catalogue from the unit, or from
`--catalog-file`): which mapped models exist on the unit, which do not (with the closest names), which
mapped parameter names exist, unit mismatches (the mapping's unit against the unit's), ranges (the
unit's range, the range a `scale`/`offset` transform gives, the placeholder 0 to 1 ranges pyquadcortex
cannot convert), and the parameters the unit has that the mapping does not use. It **does not change**
the mapping file; setting `verified: true` is done by hand afterwards.

**What to send back:** `qc-reports/qc-catalog.json` and `qc-reports/mapping-check.md`, and after a
session `match/report.md` (the recordings and analyses stay with you).

## Tests

```
python3 tools/match/test_qc_match.py
```

They use fake pyquadcortex objects and a mocked `subprocess`, so no unit, no library and no
binaries are needed. They check that the proxy refuses every save and settings call, the refusal on
a preset with unsaved edits, the plan and mapping logic, the command lines built for `PedaleiraMatch`
and `PedaleiraRender`, and that a dry run makes no grid edit and does not even connect. They also
cover the signal gain (plan parsing, the scaled copy, the commands and the report), the parameter
readback table and the clearing of leftover edits on the scratch preset.

## Matching against a plugin

`plugin_match.py` is the same flow with a plugin you own as the reference (a VST3, or an Audio
Unit on macOS) instead of a unit. It is for measuring references offline, on your computer, to tune
our own models, and for nothing else.

- **Nothing a third-party plugin produces is shipped or committed.** No IR, preset, state or audio
  from it goes into the repo. Everything is written to `--dir`, which must be outside the checkout;
  the script (and `PedaleiraMatch plugin-render`) refuses to write inside it, and the script refuses a
  plugin or state file inside it (the test suite's own PedaleiraTestFx excepted).
- You must have a licence for the plugin and check its terms. Nothing is decompiled, decrypted or
  copied; only the audio it returns is measured. No network is used.

Build `PedaleiraMatch` and `PedaleiraRender` as above. First list the plugin's parameters to write a
plan from:

```
build/tools/PedaleiraMatch_artefacts/Release/PedaleiraMatch plugin-params --plugin "/path/Some Distortion.vst3"
```

That prints each parameter's row, name, current text and number of steps (`continuous` for a
float). `--state file.vstpreset` first restores a state (a `.vstpreset`, or a raw state chunk), and
`--json` prints it as JSON. Then run the plan (a dry run unless `--go`):

```
python3 tools/match/plugin_match.py --plan tools/match/example-plugin-plan.json --dir ~/PedaleiraMatch/dist
python3 tools/match/plugin_match.py --plan tools/match/example-plugin-plan.json --dir ~/PedaleiraMatch/dist --go
```

| Option | What it does |
|---|---|
| `--plan FILE` | The takes (below). |
| `--go` | Render. Without it the script prints the plan as it will run and does nothing. |
| `--dir DIR` | Where the session goes (`signal/`, `ref/`, `ours/`, `analysis/`, `presets/`, `report.md`). Outside the repo; needed with `--go`. |
| `--tail S`, `--block N`, `--channels 1\|2` | Seconds rendered past the signal (default 4), the plugin's block size (default 512), and the channels of both sides (default: the plugin's output width). |
| `--overwrite` | Replace a reference that exists already. The default is to fail the take. |
| `--match-tool`, `--render-tool` | Other paths for the two tools. |

### The plan

```json
{
  "format": "pedaleira-plugin-match-plan",
  "version": 1,
  "plugin": {
    "path": "/Library/Audio/Plug-Ins/VST3/Some Distortion.vst3",
    "params": { "Tone": 0.5, "Level": { "text": "0.0 dB" } }
  },
  "takes": [
    { "name": "dist-gain-sweep",
      "sweep": { "param": "Gain", "values": [0, 0.25, 0.5, 0.75, 1] },
      "ours": { "kind": "drive.distortion",
                "params": { "filterPercent": 60, "levelDb": 0 },
                "sweep": { "param": "distortionPercent", "values": [0, 25, 50, 75, 100] } } }
  ]
}
```

- `plugin` (at the top, and optionally in a take, laid over it): `path` (a file, or an
  `AudioUnit:` identifier), `state` (optional file; `null` in a take drops the shared one) and
  `params`.
- A parameter is `"Name": 0.5` (normalised 0 to 1; names match without regard to case),
  `"Name": {"text": "-6 dB"}` or `"Name": "-6 dB"` (the plugin converts the text), or
  `"label": {"index": 4, "value": 0.5}` (a row of `plugin-params`). Settings apply in order, after the
  state.
- `ours`: `kind`, `params`, and the common `mix`, `inputGain`, `outputGain`, `enabled`, as in the
  Quad Cortex plan. A take without `ours` renders and analyses the reference only.
- `sweep`: `{"param": "Gain", "values": [0, 0.25, ...]}` expands into one take per value, named
  `<name>-01-<value>`, `<name>-02-<value>`. The values are numbers, texts, and `param` may be a row
  number. When `ours` has a `sweep` of its own with the same number of values (`{"param":
  "distortionPercent", "values": [...]}`), ours takes the matching value on each take, so both sides
  sweep together; otherwise ours stays fixed.
- `notes`: free text for the report.

Each reference comes with a JSON beside it (`ref/<take>.json`): the plugin's name and version, the
state, every parameter as requested and as the plugin holds it (normalised value and shown text), the
layout, and the latency that was compensated. The report is `<dir>/report.md`, in the form of the
Quad Cortex one.

Tests: `python3 -m unittest tools/match/test_plugin_match.py` (plan parsing, sweeps, the safety
checks, the command lines and a mocked session; no plugin needed). The C++ half is covered by
`match_plugin_tests.cpp` in the DSP test suite, against the test plugin.

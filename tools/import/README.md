# Preset readers

One small reader per source of presets. A reader writes the neutral "foreign
preset" JSON that Pedaleira's importer takes (`docs/import.md` has the format,
the mapping tables and the whole route). Readers are scripts, not part of the
app.

## Quad Cortex: `quadcortex_dump.py`

Reads presets from **your own** Quad Cortex over USB, with
[pyquadcortex](https://github.com/stokes-audio/pyquadcortex) (MIT, an
unofficial library that speaks the protocol the unit already speaks). It
**only reads**: the script has no call that writes, saves, deletes or moves
anything on the unit. It keeps block names, parameter values (in the units the
screen shows), scenes and routing, and nothing else.

### Set up

1. Python 3.11 or newer.
2. `pip install pyquadcortex`
3. The hidapi library: macOS `brew install hidapi` (then run with
   `DYLD_LIBRARY_PATH=/opt/homebrew/lib` in front of the command), Debian and
   Ubuntu `sudo apt install libhidapi-hidraw0`, Windows: included.
4. **Quit Cortex Control.** It holds the USB interface exclusively, and the
   script cannot connect while it runs.
5. Connect the unit by USB and switch it on.

pyquadcortex is verified by its author on CorOS 4.0.1; the protocol has no
version of its own, so a CorOS update can break it.

### Run

```
python3 tools/import/quadcortex_dump.py --current --out dumps/
python3 tools/import/quadcortex_dump.py --slot 28C --out dumps/
python3 tools/import/quadcortex_dump.py --preset "Brit 2203" --setlist factory --out dumps/
python3 tools/import/quadcortex_dump.py --all --setlist user --yes --out dumps/
```

| Option | What it reads |
|---|---|
| `--current` | The preset on the grid now, unsaved edits included. Recalls nothing. |
| `--slot 28C` | One preset by the slot name the unit shows. |
| `--preset NAME` | One preset by name (exact, case does not matter). |
| `--all` | Every preset of the setlist, in slot order. Needs `--yes`. |
| `--setlist user\|factory` | My Presets (default) or the factory library. Not used by `--current`. |
| `--out DIR` | Where the files go, one `<slot> <name>.json` each. |

**One thing to know before you run it on a unit in use.** The unit has no way to
read a stored preset without loading it, so `--slot`, `--preset` and `--all`
**recall** each preset on the unit: the audio cuts for a moment, the unit ends
up on the last preset read, and unsaved edits on the unit are discarded. Save
what you are working on first, or use `--current`.

Then, in Pedaleira:

```
preset.importForeign { "file": "/path/to/dumps/28C Brit 2203.json" }
```

The result is a grid preset in the next free slot and a report of what was
carried over, approximated, left out and dropped (`docs/import.md`). The dump
files themselves are your data: keep them out of the repository.

### Tests

```
python3 tools/import/test_quadcortex_dump.py
```

They feed the translation fake objects shaped like pyquadcortex's, so they run
without a unit and without the library installed (it is imported only when the
script talks to a unit).

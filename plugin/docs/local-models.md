# Local models: drop / picker loading

A local `.nam` file, an IR `.wav`, or a folder of them loads as a local
block, no browser or account involved: dropped on a tile, or picked via the
tile context menus' **Load File / Load Folder** (a native OS file dialog).
The design goal is that local files are a second-class *entry point*, not a
second-class block: once in, they ride the exact catalog pipeline
(background load, in-memory model cache, undo, duplication, presets, DAW
state), and the code branches on "local" in only a handful of places.

Entry points: both the drop and the picker land in
`plugin/ui/services/LocalFiles` (`drop` / `pick`), which hands a path to
`TONE3000Processor::loadLocalTonePath` in
`plugin/src/ProcessorModelLoader.cpp` (on iOS, security-scoped URLs to
`loadLocalToneUrls`; see `docs/ios.md`). Behavior is pinned by
`test/src/local_load_tests.cpp`, which drives the byte-array sibling
`loadLocalTone` ({ name, base64 data } entries: the shape the UI used before
it had file paths, kept because the tests feed files that way).

## Drop and picker

Dropping a file or folder onto a tile (`juce::FileDragAndDropTarget` on the
gallery tiles) and right-clicking a tile for **Load File** / **Load Folder**
(a native `juce::FileChooser`) are the same route with two front doors: an
insert slot adds, a tone tile swaps in place.

`loadLocalTonePath` reads the bytes straight from disk. A folder is walked
recursively; the majority extension decides NAM vs IR, the folder name
becomes the tone title, and each file becomes one model named after it
(300 files / 50 MB each max, matching the catalog's per-tone model limit,
in natural name order). A single file must be `.nam` or `.wav` and is titled
from its name.

Each file is validated at load time (`.nam` must parse, pass the
architecture check and build once; `.wav` must open as real audio) so a bad
file is a toast, never a retry badge. Survivors are stashed (below) and wrapped in a synthetic tone
JSON: `id: 0`, `local: true`, and each model's `model_url` pointing at its
stash copy with a `file://` URL. From there `loadTone` takes over, and
`fetchModelFromUrl` resolves `file://` URLs from disk instead of the
network.

While a file is open for validation, its gear is inferred best-effort so a
lane of local tiles isn't a row of identical file glyphs: a `.nam`'s
`metadata.gear_type` (free text written by the trainer; only the common
spellings map, e.g. `amp` / `pedal_amp` / `preamp` → `amp`, `amp_cab` →
`amp-cab`, `studio` → `outboard`) and, for an IR, cab length (at or under
the short/long cutoff → `cab`). The result is the same catalog `gear` id a
TONE3000 tone carries, set on the tone from its first file; when nothing
matches, `gear` is simply absent and the tile keeps the generic file glyph
(`ToneImage`).

The picker isn't sugar: it covers users who never think to drag-drop, works
signed out, and is the only route on iOS (the Files app has no drag into
the plugin). Drops work on every desktop platform, Linux included; the old
web UI never received them there because XDnD died at the embedded
WebKitGTK view
([issue #22](https://github.com/tone-3000/tone3000-plugin/issues/22)), one
of the reasons the UI is native.

## One stored model list, one exception

Catalog tones store only the *active* model natively; the picker pages the
full catalog from the API. Local tones have no API, so their model list is
the dropped files and stays whole in the stored tone JSON:
`parseToneForLoading` and `switchModel` skip their pruning for local tones,
and the tone summary ships each local model's `model_url` so the picker can
drive switches (which also work signed out; nothing downloads).

## The stash

`<app-data>/TONE3000/LocalModels/<content-hash>-<size>.<ext>` is the local
equivalent of "the server": the copy that cache-lost reloads (undo after a
remove, retry) re-fetch from, stable even after the user's original file
moves. Content-addressed names dedupe re-drops.

Its lifecycle is self-maintaining:

- **Liveness is mtime.** Every use re-stamps the file: stash writes, reads
  in `fetchModelFromUrl`, and cache-hit loads via `refreshLocalStashCopy`.
- **GC.** `cleanLocalModelStash` (once per process, off-thread) deletes
  stash files unused for a week. Clearing on startup would be wrong:
  instances in other processes may still hold undo history that references
  the files by path.
- **The name is the address.** A block's `model_url` persists the stash
  path absolutely, in presets, DAW/app state and undo snapshots. Reads go
  back through `resolveLocalModelFile`, which falls back to the same file
  name under the *current* stash root when the stored path is gone. A path
  this machine wrote always still exists, so desktop loads are untouched; the
  one desktop case it changes is a preset or state carrying a stash URL from
  another machine, which now re-stashes locally from the embedded bytes
  instead of reading the embedded cache alone. It is what keeps iOS working:
  the app data container's UUID rotates on every reinstall or app update.
- **Self-healing.** Presets and DAW state embed the model bytes
  (`ModelCache`), so they reopen without the stash, on any machine. When
  such a load hits the embedded cache and the stash copy is missing (GC'd,
  or a different machine), `refreshLocalStashCopy` writes it back, so undo
  and retry keep working there too.
- **A writable root, even after damage.** The app-data folder can exist
  without being writable: a sudo'd run of an older `install-plugin.sh`
  (it wrote the user Factory folder, and macOS sudo keeps `$HOME`) or a
  restored backup leaves it root-owned, and every stash write then failed as
  "Couldn't store the dropped file" while reads kept working
  ([issue #76](https://github.com/tone-3000/tone3000-plugin/issues/76)).
  `ensureWritableDir` (constructor, once per process, plus the stash and
  preset write paths) puts the write bits back in place when the user still
  owns the folder; otherwise it renames the folder aside to an `.unwritable`
  sibling (the parent belongs to the user even when the folder doesn't) and
  recreates it fresh. Nothing is deleted, and the log names what it did.

## Names, recents and the architecture check

The stash names are hashes, so the loader also tells `NamLibrary`
(`plugin/src/NamLibrary.cpp`) the name a file was loaded under and where it came
from. That record lives in `<app-data>/NamRecents.json`, not in the stash, whose
files are collected after a week. It feeds three things:

- `assets.list` lists a stash or asset-store copy by that name, so the block
  library's search finds a loaded capture by its file name.
- `nam.recent` is the recent list (newest first; the original file when it is
  still there, else the stash copy).
- Loading a copy again from the stash keeps its name (`touchLoaded`).

The gate in `stashLocalBytes` has two steps. `NamLibrary::inspectBytes` parses
the file with the weight arrays skipped and checks what can be checked without
them: an architecture NeuralAmpModelerCore builds (WaveNet A1 and A2, LSTM,
ConvNet, Linear, and slimmable containers of those, at any depth), a NAM file
version the core reads, settings that are complete, and a sample rate (when
the file names one) between 8 and 768 kHz. Then `NamModels::build` builds the
model once, which checks the weights against the settings. A file that fails
either is refused with a sentence that names the architecture and what is
wrong ("This LSTM capture is damaged and cannot be loaded: it holds 2 weights
where its settings need 70."). The first step backs the `architecture`,
`architectureLabel`, `a2`, `sampleRate`, `supported` and `unsupportedReason`
fields of `assets.list`, `nam.inspect` and `nam.recent`, so the library can
say so before a click.

Until 2026-10-18 only NAM A2 files loaded; that rule was reversed, so a
player's older captures play as they are.

## Architectures and the model's rate

How each architecture runs (the engine side is `NamEngine.h`, the builders
`NamModels.h`):

| Architecture | Built by | Oversampled chain | Notes |
| --- | --- | --- | --- |
| WaveNet A2, A1 | the core | phase-split | A2 takes the core's fast path |
| Linear | the core | phase-split | primed once at prepare (it does not prewarm); its source file was missing from the NAM library build until 2026-10-18 |
| ConvNet | `RealtimeConvNet` | phase-split | the core's allocates every call |
| LSTM | `RealtimeLstm` | one instance, rate lowered around it | recurrent, so it can't be phase-split; the core's allocates every sample |
| SlimmableContainer | the core, or `NamModels` when it holds an LSTM or ConvNet | phase-split when every model in it is | |

`RealtimeLstm` and `RealtimeConvNet` keep the core's weight layout and
arithmetic (the ConvNet reuses the core's own blocks) with every buffer made
when the block size is set; `NamArchitectureTest.MatchesTheCoreBitForBit` pins
them bit-identical to the core's own models. They also count the weights
before reading them: the core's LSTM and ConvNet do not, and would read past
the end of a short file.

Calibration and loudness metadata (`input_level_dbu`, `output_level_dbu`,
`loudness`) are read the same way for every architecture, so input
calibration, the calibrated hand-off between two captures and loudness
normalization work for all of them; a file without the fields gets what an A2
file without them gets. Only a slimmable container changes with the block's
size setting; a capture of one size keeps it.

A capture runs at the rate it was trained at (the file's `sample_rate`, 48 kHz
when the file has none, as NAM assumes). Run at another rate its time
constants and its frequency response move with the ratio: a 44.1 kHz capture
played unresampled in a 48 kHz chain had its resonances 8.8% high. The engine
bridges the chain rate and the model's rate:

- **none** when they are equal (every A2 capture);
- **half-band stages** when the ratio is a power of two (a 96 kHz capture, or
  an LSTM in an oversampled chain): the chain oversampler's minimum-phase
  allpass half-bands, no added latency;
- **a band-limited resampler** for any other ratio (`SincResampler.h`): a
  Kaiser-windowed sinc polyphase bank designed against the lower rate,
  passband flat within 0.0002 dB to 0.4535 of the lower rate (20 kHz at
  44.1 kHz), at least 99 dB down from 0.5465 of it, linear phase. It runs on
  the multi-port boundary's push/pop schedule and adds 1.67 ms for a
  44.1 kHz capture at 48 kHz (80 samples), which the block reports to the
  chain like any block latency: the dry share of its mix and the other paths
  are delayed to match, and the host is told.

NAM's own plugin does the same job with the Lanczos resampler in
AudioDSPTools; that kernel is not scaled when it lowers the rate, so what a
capture puts above the lower rate's Nyquist folds back. It is still what the
host boundary uses (the direction it lowers the rate in is the guitar input,
which has little up there); the model bridge uses the sinc bank because
what a capture puts out does (an 88.2 kHz capture's distortion, say).

The TONE3000 desktop app has no downloads folder: it streams models into memory and
embeds them in presets, and writes only content-addressed copies of the local files
it loaded, so there is nothing of it to look for and Pedaleira does not guess.

Captures downloaded on Pedaleira's own TONE3000 page are kept in the library:
`<library>/TONE3000/<tone title> - <capture name>.nam` (the library is
`<Documents>/Pedaleira` by default). The name is made by `NamLibrary::downloadBaseName`
and is safe on every OS: the characters Windows refuses (`< > : " / \ | ? *`) and
control characters become a hyphen, runs of spaces and hyphens shrink to one, a leading
or trailing dot or space is dropped, a reserved Windows device name (`CON`, `NUL`,
`COM1` and so on) gets a `TONE3000 ` prefix, the name is cut to 120 characters, and
`TONE3000 capture` stands in for an empty one. A name already taken gets ` (2)`, ` (3)`
and so on, never an overwrite. Beside the files, `downloads.json` remembers for each the
tone id, the capture id and the credit (`tone3000`: tone, creator, licence), so:

- the files persist across restarts and show in `assets.list` (`origin` `tone3000`,
  with `tone3000`), in search and, once loaded, in `nam.recent`;
- a capture already downloaded is reused (`nam.downloaded {tone, capture}`), not
  downloaded again.

The TONE3000 page downloads to a temporary file and hands it to `nam.keepDownload`
(`{file, capture, tone3000}`, local only), which moves it into place and records it.

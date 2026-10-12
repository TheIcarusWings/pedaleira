# The setlist library

Presets live in a folder of plain files that can be backed up, copied to
another machine and versioned with git. This is the engine side (the Library
page of the app is its UI), and everything is
reachable through the command layer
(`docs/commands.md`: `setlist.*`, `preset.*`, `library.*`, `favourite.set`,
`recent.list`). The code is `plugin/include/SetlistLibrary.h` (folders and
files), `plugin/include/PresetJson.h` (the preset format and the asset
store), `plugin/src/ProcessorLibrary.cpp` (the processor's side) and
`plugin/src/LibraryCommands.cpp` (the commands).

## Folder layout

The library root is `Pedaleira` in the user's Documents folder. Nothing is
created there until something is saved. The root is resolved in one
function, `LibraryPaths::libraryRoot()`; the setlists, the asset store and
the agent layer's guitar profiles (`docs/agents.md`) all hang off it.

```
Documents/Pedaleira/
  library.json                     favourites, recents, position, setlist order
  Setlists/
    Friday Gig/                    one folder per setlist
      setlist.json                 the setlist's id and name
      001 Intro Pad.json           one file per preset, slot number first
      002 Crunch Rhythm.json
      012 Glass Cleans.json        slots may be empty: 3 to 11 are, here
    Covers/
      setlist.json
      001 Sultans.json
  Assets/
    7e50b41e...4590.wav            model files, named by their SHA-256
    c41d09aa...77e2.nam
  Guitars/                         guitar profiles (docs/agents.md)
    guitars.json                   which one is the default
    les-paul/
      profile.json
      bridge-soft.wav              the recorded DI takes
  Trash/                           only where the system has no trash
```

`Setlists`, `Assets`, `Guitars`, `Trash` and `library.json` are the names
the app uses in the root; nothing else is read or written there. Renders an
agent asks to keep and other throwaway files are not in the library at all:
they go to a scratch folder of the user's own, `<app data>/Pedaleira/Scratch`
(`LibraryPaths::scratchDir()`; not the system's temp directory, which other
accounts can write to). A command server client may name files
inside the root (and the other allowed folders, `docs/agents.md`), which is
what lets it import a preset from there or export one into it.

- **A setlist is a folder.** Its display name and its id are in
  `setlist.json`; the folder name is the name made safe for file systems.
- **A preset is one JSON file** in a setlist's folder. The file name is
  `NNN Name.json`: the slot (1 to 256, three digits) and the preset's name
  made safe for file systems. The slot is only in the file name; the name and
  the id are inside the file, and the name inside is the one that counts.
- **A setlist holds up to 256 presets.** Slots may be empty. A preset's slot
  is its place everywhere: MIDI program change `n` (0 to 127) loads slot
  `n + 1`, the footswitches' preset banks are slots 1 to 8, 9 to 16 and so on,
  and an empty slot is an empty program and an unlit switch.
- **Assets are shared.** Model files are stored once in `Assets`, however
  many presets and setlists use them.
- **The factory presets are not in the library.** They stay where the
  installer puts them (`.t3kpreset` files) and appear as a read-only setlist
  with the id `factory`, always last. `preset.copyToSetlist` copies one into
  a setlist of your own.

### Names

A name becomes a file or folder name that is valid on macOS, Windows and
Linux at once, so a library made on one opens on the others:

| Rule | Example |
|---|---|
| `/ \ : * ? " < > \|` and control characters become `-` (runs collapse) | `AC/DC` is `AC-DC` |
| Leading and trailing spaces and dots go; a leading `.` or `~` goes | `  Lead... ` is `Lead` |
| Windows device names get a `_` in front | `CON` is `_CON` |
| At most 100 bytes of UTF-8, cut between characters | |
| Invisible characters (zero width, direction marks) go | |
| Unicode is composed (NFC), so `é` typed on a Mac and on Linux is one name | |
| Nothing left: `Preset` or `Setlist` | `???` is `Preset` |

The real name is never lost: it is in `setlist.json` or in the preset file.
Two setlists may not have names that differ only in case or composition
(`Blues` and `blues`); two setlists whose names clean to the same folder get
` 2`, ` 3` appended to the folder. Presets never collide, because the slot
number is part of the file name.

Composition and case folding are done by the app's own tables, not by the
operating system, so the result is the same on every machine. Composition
covers the pairs of the Basic Multilingual Plane plus Hangul, joining
neighbours in the order they appear; case folding covers ASCII, Latin-1,
Latin Extended-A, Greek and Cyrillic.

### Changes made outside the app

The folders are the truth. The app keeps no index: every listing reads the
folder, with a cache of what each file said keyed on its time and size. So a
preset added, removed, renamed or edited in a file browser, a text editor or
by git is simply what the app sees next.

A listing never writes. What it finds is treated like this:

| Found in a setlist folder | Treated as |
|---|---|
| `012 Name.json`, a preset | The preset of slot 12 |
| `Name.json`, a preset without a number | A preset in the first empty slot |
| Two presets with the same number | The first by file name keeps the slot, the other gets the next empty one |
| A preset whose name inside differs from its file name | Listed under the name inside |
| A preset without an `id` | Listed under an id made from its file name; given a real one the first time the app writes it |
| A copy of a preset (same `id`) | Listed under `<id>~2` until the app writes it |
| A `.json` that is not a preset (broken, another format, a merge conflict) | Ignored and left alone; `setlist.get` names it in `ignored` with the reason |
| A `.json` larger than 64 MB, or with arrays and objects nested more than 64 deep | Not parsed at all; ignored and left alone, named in `ignored` with that reason |
| `012 moving-1a2b3c4d Name.json` | A preset on its way to slot 12 when the app was cut short while renumbering: the preset of slot 12 (see "How files are written") |
| Any other file or folder | Not looked at |
| More presets than slots | The ones without a slot are named in `ignored` |
| A folder without `setlist.json` | A setlist named after the folder; it gets a `setlist.json` the first time the app writes into it |
| A copied setlist folder (same id) | The first by folder name keeps the id |

Files are renamed only by an edit of that setlist (which first gives every
preset file of the setlist its proper name) or by `library.rescan` with
`tidy`. The app never deletes a file of the library itself:
`setlist.delete`, `preset.delete` and `preset.deleteSlot` hand files to the
system trash, or move them to `Trash` in the library root where the system
has none (but see "Known limits" for volumes without a recycle bin).

### What is read, and what is refused

Every JSON file of the library is read through one guarded reader, so a file
that is broken, huge or made to do harm costs a preset at most, never the
app:

- A preset file may be at most 64 MB (one exported with its assets inside
  is megabytes; nothing the app writes comes near the limit), `library.json`
  and `setlist.json` at most 1 MB. A larger file is not read.
- Arrays and objects may nest at most 64 deep (a real preset stays under
  20). A file that nests deeper is not parsed.
- A file refused for either reason is treated like any other file that is
  not a preset: ignored, left alone and, in a setlist's folder, named in
  `ignored` with the reason. `preset.importJson` and `--preset` of the
  render tool fail with the same reason.
- A number a file gives as NaN or an infinity (the format can say them, see
  "How the tree is written") never reaches the rig: on load a block's gains,
  mix, size, EQ and parameters and the faceplate parameters take their
  default instead. The file itself is not changed, and converting it to a
  `.t3kpreset` and back still keeps what it said.

`library.rescan` forgets the cache. It is only needed for an outside change
that keeps a file's size and happens within the file system's time
resolution of the last look.

## The preset format

A preset file is the saved rig as text: `"format": "pedaleira.preset"`,
`"version": 1`. It holds exactly what a `.t3kpreset` holds, so converting
one into the other and back gives the same file, and both restore to the
same plugin state byte for byte.

```json
{
  "format": "pedaleira.preset",
  "version": 1,
  "schemaVersion": 1,
  "name": "Glass Cleans",
  "id": "ee729a0f58ac437b94318f5f9cd11b9f",
  "chain": {
    "stereoEnabled": false,
    "branchSide": "left",
    "branchAfterBlockId": "",
    "children": [
      {
        "node": "ChainBlocks",
        "children": [
          {
            "node": "ChainBlock",
            "id": "55efdcd52d4b4d68afcbf29aa87b7c85",
            "type": "ir",
            "enabled": true,
            "normalize": true,
            "slimSize": 0.0,
            "inputGain": 0.5,
            "outputGain": 0.5,
            "mix": 1.0,
            "toneId": 0,
            "toneJson": "{ ...the tone as the block keeps it... }",
            "activeModelId": 664088170,
            "children": [
              {
                "node": "Eq",
                "enabled": true,
                "pre": false,
                "children": [
                  { "node": "Band", "type": "lowshelf", "freqHz": 100.0, "gainDb": 0.0, "q": 0.7099999785423279 }
                ]
              },
              {
                "node": "ModelCache",
                "children": [
                  {
                    "node": "CachedModel",
                    "modelId": 664088170,
                    "data": { "$asset": "7e50b41ea90be782073d1f3c94772395b8b008d521614c5b7b1d7b72f2674590", "ext": "wav", "name": "cab-ir-test", "size": 72080 }
                  }
                ]
              }
            ]
          },
          {
            "node": "ChainBlock",
            "id": "51ea89d073794719b935490c52b3bece",
            "type": "kind",
            "enabled": true,
            "mix": 0.3499999940395355,
            "children": [
              {
                "node": "KindState",
                "kind": "delay.simple",
                "version": 1,
                "name": "Simple Delay",
                "children": [
                  { "node": "Param", "id": "timeMs", "value": 350.0 },
                  { "node": "Param", "id": "feedback", "value": 35.0 }
                ]
              }
            ]
          }
        ]
      },
      { "node": "RightChainBlocks" },
      {
        "node": "Scenes",
        "version": 1,
        "active": 0,
        "children": [
          { "node": "Scene", "index": 0 },
          { "node": "Scene", "index": 1, "name": "Chorus" }
        ]
      }
    ]
  },
  "params": {
    "inputLevel": 0.5,
    "toneBass": 6.5,
    "gateThreshold": -80.0
  }
}
```

(Shortened: a real file lists every slot of both lanes, all six EQ bands,
all eight scenes and all the faceplate parameters.)

| Key | What it is |
|---|---|
| `format`, `version` | Always first. A file with a higher version is refused, not guessed at. |
| `schemaVersion` | The rig's own: 1 for a legacy rig, 2 for a grid. |
| `name`, `id` | The preset's name and its id (shown by the commands as `user:<id>`). |
| `tempo`, `tags` | Optional. No save of this build writes them, but they are read: `tempo` (BPM) becomes the rig's tempo when the preset loads, and both show in `setlist.get`. Top-level keys the rig does not write (these, or a note of your own) are kept when the preset is renamed, moved, copied or saved over. |
| `chain` | The chain snapshot. A legacy rig: `stereoEnabled`, the branch, `ChainBlocks` (left lane) and `RightChainBlocks`. A grid: `"layout": "grid"` and a `Grid` node with four `Row` nodes (each with `Input`, `Output` and `Slots`) and the `Split` nodes; a row feeding another row is its `Output` with `"type": "row"`. Then, when used, `Scenes` and `Stomps` (`docs/scenes.md`, "Saved shape"). |
| `ChainBlock` | One slot: its `id`, `type` (`nam`, `ir`, `kind` or `insert`), the common settings, `Eq`, and either its tone (`toneId`, `toneJson`, `activeModelId`, `ModelCache`) or its `KindState` (the kind id and the kind's parameters; for a hosted plugin its identity and state `chunk`). |
| `params` | The faceplate parameters a preset carries, by id, in real units. |

### How the tree is written

The mapping is mechanical and is the same for every node, so a block kind
added later, or a node this build does not know, is written and read back
without the format changing:

- A node is an object: `"node"` (its type), its properties in the order the
  rig saves them, then `"children"`. Nothing is sorted and nothing is
  dropped, which is what makes the round trip exact.
- At the top level the `ChainSnapshot` node is the `chain` key and the
  parameters are the `params` map. (A file whose top level is not that shape
  is written with `children` like any node.)
- A property named like one of the format's own keys (`node`, `children`,
  and at the top `format`, `version`, `chain`, `params`), or starting with
  `$`, is written with one more `$` in front.
- Values keep their kind. A whole number is an int, a number with a decimal
  point or exponent is a double (`1` and `1.0` are different values). The
  few values JSON cannot say are objects with one `$` key:

| Written as | Is |
|---|---|
| `{ "$asset": "<sha256>", "ext": "nam", "name": "...", "size": 123 }` | Binary data in the asset store |
| `{ "$base64": "..." }` | Binary data in the file |
| `{ "$int64": "5" }`, `{ "$int": "-2147483648" }` | An integer whose width JSON would not keep |
| `{ "$double": "nan" }`, `"inf"`, `"-inf"`, `"0x<16 hex digits>"` | A double with no decimal text (the last: below 1e-308) |
| `{ "$undefined": true }` | An undefined value (`null` is a void one) |

### Formatting

The text of a given rig is always the same bytes, and a small change to the
rig is a small diff:

- two space indent, `\n` line ends, UTF-8 without escapes for anything but
  control characters, one final newline;
- keys in saved order (never reordered between saves);
- one property per line, except that a node without children that fits in
  100 characters is one line (a kind parameter, an EQ band, a scene's block
  state);
- the `params` map and every array are one entry per line;
- a double is the shortest decimal text that reads back as exactly the same
  value: `0.5`, `380.0`, `0.3499999940395355` (most settings are 32 bit
  floats inside the engine, and that is their exact value). Saving a preset
  that was loaded and not changed rewrites the same bytes.

`toneJson` is a JSON text inside a string, exactly as the block keeps it.
It is not unfolded, because folding it back could not promise the same
bytes.

## The asset store

`Assets/<sha256>.<ext>`: the lower case hex SHA-256 of the bytes, and an
extension for people (`nam`, `wav`, `bin`). A preset names an asset by hash,
with the name the bytes had and their size.

- **Model files always go to the store** (the NAM capture or IR of a block),
  whatever their size.
- **Other binary values**, which today means a hosted plugin's state chunk,
  go inline as base64 up to 2048 bytes and to the store above that. Most
  plugins with a handful of settings stay inline, so the preset shows the
  change in a diff; a plugin with megabytes of state does not bloat it.
- **Stored once.** Saving a second preset with the same capture writes no
  new file. Copying a preset to another setlist copies no asset. A file
  that is already there is first checked against its hash (once, then again
  only when its time or size changes); one that is not the bytes its name
  says, a damaged copy of the right size for instance, is written again.
- **Checked on load.** A file whose size or hash is not what the reference
  says is not used. A file that has passed is not hashed again until its
  time or size changes, so switching presets does not re-read it for the
  check.
- **Never deleted by the app.** Deleting a preset leaves its assets, since
  another preset (or another branch) may use them. Unused assets can be
  removed by hand: an asset no preset file mentions is unused.

### A missing asset

A preset whose asset is missing, or damaged, still loads:

- A block whose **model** is missing comes up failed (`loadFailed` in the
  chain state) under its name and in its place, and the rest of the rig
  plays. When the tone names a download or a file this machine has, the
  engine fetches it from there as it always did.
- A block whose **plugin state** is missing comes up as a bypassed
  placeholder under the plugin's name (`missing` in the chain state), not as
  the plugin on default settings.
- The reference is kept. Renaming, moving, copying or exporting the preset
  writes it back, and so does saving the rig while a plugin block is a
  placeholder. The one exception: saving the rig over a preset whose
  **model** is missing writes the block without that model, as the engine
  no longer has the bytes.

`preset.importJson` reports the assets a file names that nobody has in
`missingAssets`.

## library.json

One small file in the library root:

```json
{
  "format": "pedaleira.library",
  "version": 1,
  "setlistOrder": [
    "b1536150aa964921ba4b1b16a112000a"
  ],
  "current": {
    "setlist": "b1536150aa964921ba4b1b16a112000a",
    "slot": 12
  },
  "favourites": [
    "user:ee729a0f58ac437b94318f5f9cd11b9f"
  ],
  "recents": [
    "user:ee729a0f58ac437b94318f5f9cd11b9f"
  ]
}
```

- `setlistOrder`: setlist ids in list order. Setlists it does not name
  follow by name.
- `current`: the setlist the preset list shows and the slot loaded last in
  it. Each running instance keeps the position it chose itself, so two
  plugin instances in one project do not move each other's program numbers;
  the file is where the next launch starts.
- `favourites`: preset ids, sorted. A factory preset can be one.
- `recents`: preset ids, most recent first, at most 32, updated when a
  preset is loaded. Ids of presets that are gone stay in the file and are
  left out of `recent.list`.

Keys the app does not know are kept.

A missing or empty `library.json` is an empty state: no favourites, no
recents, no position, setlists in name order.

A `library.json` that is there but is not a library state (it cannot be
read, is cut short, holds merge conflict markers, is not a JSON object, is
larger than 1 MB or nests more than 64 deep) is handled like this:

1. Reading never touches it. The app works from the empty state in memory:
   the setlists and presets are all there, favourites and recents are
   empty, the setlists are in name order.
2. The first time the app has something to write to it (a preset is
   loaded, a favourite set, a setlist created or reordered), it first
   copies the broken file, byte for byte, to
   `library.json.broken-<YYYYMMDD-HHMMSS>` beside it (`-2`, `-3` appended
   when that name is taken). If the copy cannot be made, nothing is
   written and the broken file stays as it is.
3. Then `library.json` is replaced by the state the app has in memory, and
   from there on it is an ordinary file again. A copy is made once per
   broken file: the same bytes are not copied a second time.

So nothing is lost without a copy: what the broken file held (favourites,
recents, the setlist order) is in the `.broken-` file, to be repaired by
hand and put back as `library.json` while the app is closed. The app never
reads or deletes a `.broken-` file.

A session a host restores is not a choice of setlist: the instance shows
the setlist its active preset is in, whatever `current` says, and restoring
it does not write `library.json`. That is what keeps a host replaying the
session's program number from loading a preset of another setlist.

Which setlist is current: the selected one while it exists, else the first
of the user's own, else the factory setlist. `preset.save` with the factory
setlist current saves into the user's first setlist and makes one called
"My Presets" when there is none.

**Unsaved changes** (`unsaved` in `library.get`) is not stored. It is worked
out when asked: the rig as an undo snapshot sees it, and the faceplate
parameters, compared with what they were when the active preset was loaded
or saved. Undoing an edit makes the rig clean again. Switching scene counts
as a difference until the preset's own scene is selected again. With no
active preset it means "not the empty default". After a session is restored
by a host, the session's rig is what the flag is measured from.

## Using git

The library is made to be a repository:

```
cd ~/Documents/Pedaleira
git init
printf 'Trash/\n' > .gitignore
printf '*.json text eol=lf\nAssets/* binary\n' > .gitattributes
git add .
git commit -m "My tones"
```

- **Diffs are small.** A knob moved is a line or two; a preset renamed or
  moved to another slot is a file rename that git follows.
- **Assets are binary and never change.** A file in `Assets` is only ever
  added. They suit Git LFS well (`git lfs track "Assets/*"`); without it
  they are ordinary binary files of 100 kB to a few MB.
- **Commit `Assets` with the presets.** A preset whose asset was not
  committed loads with that block failed (see above), and is whole again
  once the asset is there.
- **`library.json` changes often**, since loading a preset updates the
  recents and the position. Commit it to carry favourites and the setlist
  order between machines, or add it to `.gitignore` to keep history quiet;
  the library works either way.
- **After a pull, checkout or merge** the app sees the new state the next
  time it lists a setlist. Use `library.rescan` when in doubt.
- **Merge conflicts.** Two branches that put different presets into the same
  slot merge into two files with the same number: both are listed, the
  second in the next empty slot, and `library.rescan` with `tidy` renumbers
  the file. A conflict inside one file leaves conflict markers, which make
  it "not valid JSON": the file is ignored, named in `setlist.get`, and left
  for you to resolve. A conflict in `library.json` is an empty library state
  until resolved; the setlists and presets are unaffected. Resolve it
  before using the app if you can: the app's next write to the file puts
  the conflicted one aside as `library.json.broken-<timestamp>` and starts
  a new one (see "library.json").
- **Line ends.** The app writes `\n` and reads either. The `.gitattributes`
  line above keeps Windows checkouts from rewriting the files.

## What is not stored

In a preset, on purpose (as before the library):

- the input mode, calibration, oversampling and the machine's audio setup;
- the MIDI map, the footswitch mode and its hybrid rows;
- solo (monitoring, not tone);
- the rig's tempo, unless a `tempo` key is written by hand;
- undo history, editor size, anything about the window;
- the plugin binaries a rig hosts (a preset names the plugin and carries its
  state; a machine without the plugin shows a placeholder).

Kept, but tied to the machine that saved it: the `toneJson` of a tone loaded
from a local file names that file's path on that machine. The model itself
is in the asset store, so the preset loads anywhere; the path only shows up
as text in the file.

In the library folder: the factory presets, the presets of the old store
(below), logs, settings and the plugin scan cache are all elsewhere.

## Presets from before the library

Older builds kept user presets as `.t3kpreset` files in one folder under the
app data folder, and the preset list was that folder's presets followed by
the factory presets: host program `n` and MIDI program change `n` were entry
`n` of that list.

**The first time the app uses a library that has no setlist of the user's
and no record of this having been done, it brings the old list over by
itself.** A setlist called "My Presets" is made and entry `n` of the old
list becomes its slot `n + 1`, in the old order, factory presets included,
so program `n` loads the preset it always did:

- A preset of the user's keeps the id it always had (`user:<id>`), so a
  favourite or a host session that names it still finds it.
- A factory preset is copied in as a preset of the user's under a new id;
  the original stays in the read-only factory setlist.
- An old preset that cannot be read leaves its slot empty; the others keep
  their numbers. Past 256 presets a second setlist, "My Presets 2", takes
  the rest (programs only reach 128).
- The old files are not moved, changed or deleted.
- That it was done is recorded as `"legacyPresetsMigrated": true` in
  `library.json`. With the record there it is never done again, so a preset
  or a setlist deleted afterwards does not come back.
- It is not done at all when the old store holds no preset of the user's
  (a first install: nothing is created in Documents), or when the library
  already has a setlist (one made by an earlier build, or pulled from a
  repository).
- It can take a while for a long list with large captures (every model is
  hashed and stored once); the log says when it starts, how many presets
  there are and how long it took.

`library.get` says how many presets of the old store are not in the library
(`legacyPresets`: none after the migration, until one is deleted), and
`library.importLegacyPresets` copies those into a setlist as JSON presets
under the ids they always had. Importing twice imports nothing twice.

`preset.importJson` and `preset.exportJson` read and write single preset
files anywhere, in either format (a path ending in `.t3kpreset` is the old
one). An exported JSON preset has its assets in an `Assets` folder beside
it, or inside the file with `embedAssets`. The export does not write through
an `Assets` that is a symbolic link: it fails and says so.

## How files are written

- **One step, or not at all.** A preset, a `setlist.json`, `library.json`,
  an asset and an exported preset are each written to a temporary file
  beside the target (`<name>.<hex>.tmp`), and only when every byte is
  written and flushed does that file take the target's place, in one atomic
  rename. If anything fails (a full disk, a locked file) the file that was
  there is still there, whole, the temporary file is removed and the command
  reports the failure. The app never deletes a file to make room for its
  replacement.
- **Renumbering.** Moving presets between slots renames several files.
  Each first gets a scratch name that starts with the slot it is going to,
  `012 moving-1a2b3c4d Name.json`, and then its final name. If the app is
  cut short in between, the scratch files are still presets and the next
  listing puts each in the slot it was meant for; the first edit of the
  setlist, or `library.rescan` with `tidy`, gives them their proper names.
  (Cut short during the first of the two passes, the presets not yet
  renamed are still under their old numbers, and two files may then claim
  one slot: both are listed, the second in the next empty slot.)
- **A new setlist gets a folder of its own.** A folder that is already
  there is never moved into, also when the file system takes two names for
  one that the app's own rules tell apart: the new folder gets ` 2`.
- `*.tmp` files left by a crash are never read and can be deleted.

## Known limits

- **Deleting on a volume without a recycle bin (Windows).** `setlist.delete`,
  `preset.delete` and `preset.deleteSlot` hand the file to the system trash.
  On Windows a volume may have no recycle bin (most network shares, some
  removable drives, a drive with the bin switched off), and the system then
  deletes the file for good instead of keeping it, without telling the app.
  The app does not try to detect this. If the library is on such a volume,
  treat deleting a preset or a setlist as final, and keep the library under
  git or another backup. The library's own `Trash` folder is only used
  where the system reports that it has no trash at all.
- **Assets are never collected.** See "The asset store": unused model files
  stay until removed by hand.
- **A broken `setlist.json`** makes its folder a setlist made by hand: it
  is listed under the folder's name, and the first write into it gives it a
  new `setlist.json` with a new id (its place in `setlistOrder` is lost;
  its presets are untouched).
- **A program number is a slot of one setlist.** A host session stores the
  active preset, not the setlist it was in. A session whose active preset
  is no longer in any setlist (deleted, or a factory preset or a file)
  restores with the setlist selected at that moment, and a host replaying
  an old program number then loads that setlist's slot.

## The render tool

`PedaleiraRender --preset` takes a `.json` preset as well as a `.t3kpreset`
and renders the same audio from both, bit for bit. A JSON preset's assets
are looked for in the file, in an `Assets` folder beside it and in the
library the file sits in. The tool's own library is an empty scratch folder
unless `--library <folder>` names a library root, so a script's library
commands never reach the library in Documents.

# Agents: the command server, permissions, sandbox rigs and the listen tools

Pedaleira is meant to be driven by agents as well as by people: the user describes a tone
over an MCP connection and the application builds it, for that user's own guitar. This
page is the engine side of that. Nothing here has a screen yet; the last section lists
what the UI will need.

The pieces, bottom up:

| Piece | File | What it is |
|---|---|---|
| Command layer | `CommandRegistry`, `EngineCommands` | Every state change as a named command with checked JSON arguments (`docs/commands.md`). |
| Agent layer | `AgentHub` | Sessions, the permission policy, sandbox routing, the approval queue, the activity log. |
| Sandbox rigs | `SandboxRig` | A second, unheard rig to build and measure in. |
| Listen tools | `ListenCommands`, `AudioMeasure` | `render.clip`, `render.compare`, `render.probe`: audio in, numbers out. |
| Guitar profiles | `GuitarProfiles`, `InputRecorder` | A guitar as its recorded DI clips and their measurements. |
| Capture | `CaptureStudio`, `CaptureAudio`, `TrainerRunner` | Recording a device and training a NAM A2 model of it (`docs/capture.md`). |
| Command server | `CommandServer` | The command layer on a local socket. |
| Headless host | `tools/headless_host.cpp` | The engine with its command server and no window or audio device. |
| MCP server | `mcp/` | The commands as MCP tools, plus the tone tools. |

## The command server

Off by default. `server.setEnabled` (a local command, so in practice a settings switch)
starts and stops it and keeps the choice in `<app data>/Pedaleira/agents.settings`; the
standalone application brings it up at start when the setting is on. A plugin instance
inside a host never listens.

### Connection file

On start the server writes `<app data>/Pedaleira/command-server.json` (on macOS
`~/Library/Application Support/Pedaleira/`, on Windows `%APPDATA%\Pedaleira\`, on Linux
`~/.config/Pedaleira/`), readable by the user only, and removes it on stop:

```json
{
  "protocol": 1,
  "transport": "tcp-ndjson",
  "host": "127.0.0.1",
  "port": 53124,
  "token": "64 hex characters, new at every start",
  "pid": 4242,
  "started": "2026-10-10T21:15:02.120+01:00"
}
```

How it is made, since it holds the token:

- **macOS and Linux.** The folder is the application's own (`Pedaleira`, never the shared
  folder above it): made with mode 0700 when it is missing, and tightened to 0700 when
  an earlier version left it wider. The file is written under a temporary name that
  cannot be guessed, opened with `O_CREAT | O_EXCL | O_NOFOLLOW` and mode 0600, and
  renamed over the final name when it is whole. It is therefore never made through a
  link or in a file somebody else put there, never readable by others for a moment, and
  a reader sees the old file or the whole new one.
- **Windows.** The file is made in place. It has no mode to set: what keeps other
  accounts out is the access control list of the user's profile, which `%APPDATA%` and
  everything made under it inherit.
- **A file that is already there** is replaced: one left by a process that has gone
  (a crash), and also one from an instance that is still running, whose clients keep
  their connections but which new clients no longer find.
- **On stop** the file is removed only if it still names this server (its `pid` and its
  token). One that another instance has written since is left alone.

A client should check that `pid` is a running process before it trusts the port (the
MCP server does): after a crash the port is nobody's, or somebody else's.

### Wire protocol

TCP on 127.0.0.1, UTF-8, one JSON object per line in both directions.

```
client  {"auth":"<token>","client":"<a name for the client list>"}
server  {"ok":true,"result":{"protocol":1,"app":"Pedaleira","sessionId":3,"client":"..."}}

client  {"id":1,"command":"chain.addBlock","args":{"kind":"delay.simple"}}
server  {"id":1,"ok":true,"result":{"revision":7,"blockId":"...","block":{}}}

client  {"id":2,"command":"block.setParam","args":{"blockId":"nope","param":"mix","value":0.5}}
server  {"id":2,"ok":false,"error":{"code":"not_found","message":"no block with id 'nope'"}}
```

- The first line must be the token. Anything else, a wrong token, or nothing within five
  seconds closes the connection (`unauthorized`).
- `id` is echoed as given (a number or a string) and may be left out. Requests on one
  connection are answered in order; a connection handles one at a time.
- Error codes are the engine's (`docs/commands.md`), plus the transport's own:
  `unauthorized`, `invalid_request` (a line that is not a request, or one whose arrays
  and objects nest more than 32 levels deep, which is refused before it is parsed; eight
  in a row close the connection), `too_large` (a line over 1 MiB; closes),
  `rate_limited` (more than 400 requests in a second; closes), `unavailable` (too many
  connections) and `failed` without an `id` (the server hit an internal error serving
  this connection, running out of memory included; it closes this connection and
  carries on).
- Connections: eight clients at most, a client being a connection that has presented
  the token. Connections that have not yet are counted apart, two at a time: when both
  places are taken and another connection arrives, the one that has waited longest is
  closed, once it has had 300 ms to speak. Sockets that connect and say nothing can
  therefore never keep a client out.
- Numbers are written with at most nine decimals.
- After `events.subscribe` the server pushes `{"event":"revision","rig":"live","revision":N}`
  whenever the revision of the rig the connection is working on changes (or the
  connection switches rigs). Event lines have no `id`.

### Threads

One thread accepts, one serves each connection. Neither touches the processor: a request
is handed to the message thread (`AgentHub::runFromAnyThread`), where every command has
always had to run, and the answer is written back. A slow command (a render, waiting for
loads, listing assets) does only its quick part on the message thread and the rest on its
own connection's thread, so it delays that client and nobody else. Revision events come
from one atomic read per 100 ms per connection. The audio thread is not involved at all.

Stopping never waits for a client. Each session has a cancel flag
(`AgentSession::cancelled`), set when the server stops, when the application quits,
when the session is disconnected (`agents.disconnect`) or closed. Every slow command
reads it between steps, on the rig being played as on a sandbox
(`state.waitUntilLoaded`, the renders, `assets.list`), and so does a wait for the
message thread made on the session's behalf: a client sitting in
`state.waitUntilLoaded` with a ten-minute timeout answers `unavailable` at once, and
`CommandServer::stop` returns in well under a second. An exception on a connection's
thread closes that connection (`failed`); it never leaves the thread.

The server is written against a small `CommandTransport` interface (accept a connection,
receive a message, send a message). There are two implementations: the TCP line
transport above, and a WebSocket one for a browser remote.

### WebSocket transport

`makeWebSocketTransport(allowedOrigins)` (`WebSocketTransport.cpp`) carries the same
messages, one JSON object per WebSocket text message (RFC 6455), with the same token
first, the same limits and the same errors. The connection file says
`"transport":"websocket"`.

- **Loopback only**, like the TCP one. The upgrade is read on the connection's own
  thread inside the auth wait, never on the accepting thread, so a slow client keeps
  nobody else waiting; headers over 8 KiB are refused (431).
- **Host** must be `127.0.0.1:<port>` or `localhost:<port>` (403 otherwise), against
  DNS rebinding.
- **Origin**: none (a native client) or one of `allowedOrigins`, compared exactly (403
  otherwise). The list is empty by default, so every browser page is refused until the
  remote's own origin is named.
- **Frames**: text only (binary closes with 1003); client frames must be masked, with
  no reserved bits, and control frames short and whole (1002 otherwise); fragmented
  messages are joined and must be UTF-8 (1007). A message whose length goes over the
  request limit is refused (`too_large`) from its header, before its payload is read.
  Pings are answered, close frames echoed. No extensions, no compression.

Not decided yet, and needed before a phone can use it: serving the remote's page,
listening beyond loopback, and how a phone gets the token (pairing). The server also
has one connection file, so TCP and WebSocket side by side need a second file or a
transport that carries both.

## Permissions

Every command declares a permission class (`CommandPermission`, shown by
`commands.describe`; a test fails for a command without one). The policy has one switch
per class:

| Class | Switch | Default | Commands | What it covers |
|---|---|---|---|---|
| `read` | read library, presets and rig state | on | 43 | `state.get`, `preset.list`, `setlist.get`, `library.get`, `guitar.list`, `assets.list`, `nam.inspect`, `nam.recent`, `capture.get`, `capture.list`, `kinds.list`, meters |
| `edit` | create and edit presets and rigs | on | 86 | `chain.*`, `block.*`, `scene.*`, `param.set`, `preset.save`, `preset.moveSlot`, `preset.importJson`, `preset.exportJson`, `setlist.create`, `favourite.set`, `library.rescan`, `sandbox.create`, `guitar.create` |
| `render` | render audio clips to measure a tone | on | 3 | `render.clip`, `render.compare`, `render.probe` |
| `live` | change the rig being played | off | 2 | `sandbox.applyToLive`, `guitar.record.start`, and any `edit` command aimed at the live rig |
| `delete` | delete presets, setlists and guitar profiles | off | 4 | `preset.delete`, `preset.deleteSlot`, `setlist.delete`, `guitar.delete` |
| `settings` | audio, device and machine-wide settings | off | 6 | `footswitch.setMode`, `footswitch.setHybridRows`, `plugins.scan` and its folders |
| `local` | never available to clients | n/a | 38 | `approvals.list/resolve`, `agents.*`, `permissions.set`, `server.*`, `roots.add/remove`, `nam.clearRecent`, the user's side of `toneRequest.*`, the rest of `capture.*` (audio hardware and a long child process, `docs/capture.md`) |

What "edit" means while "live" is off: an agent builds and edits in a sandbox rig. A rig
command that would change the rig being played is not refused but queued for approval
(below), and so is `sandbox.applyToLive`. `delete` and `settings` commands are refused
outright while their switch is off.

The gate, in order, for a client's request: the command exists; it is not `local`; its
class is switched on; every path argument lies inside an allowed folder; if it changes
the live rig and `live` is off, its arguments are validated and it is queued. In-process
callers (the UI, tests, the headless host's stdin) use the local session and are not
gated.

The policy is kept in `agents.settings` (`permissions.get`, `permissions.set`).

### Approvals

A client's request that needs approval answers

```json
{"ok":false,"error":{"code":"needs_approval","message":"...","approvalId":"approval-4"}}
```

and waits in the queue. The client polls `approvals.status {id}` (it sees only its own
requests). The user's side is `approvals.list` and `approvals.resolve {id, approve}`,
both local: approving runs the command exactly as it was asked for, once, and keeps its
result for the client to read. A request expires after ten minutes or when its client
disconnects; a client can have four waiting and there can be sixteen in all; asking for
the same thing twice is one request. `AgentHub::onApprovalsChanged` is the hook for a
prompt.

What an approval covers: the command and its arguments, as `approvals.list` shows
them, and for `sandbox.applyToLive` the rig itself. The sandbox's rig is captured when
the request is made (`SandboxRig::capture`: the chain with its scenes and the
preset-managed parameters), kept with the request, and that captured rig is what an
approval applies. Nothing the client does to the sandbox between the question and the
answer changes it: edits are not applied, and a discarded sandbox is still applied as
it was. The request shows `applies: { sandbox, revision, captured, sandboxChanged }`;
`sandboxChanged` tells a prompt that the sandbox is no longer the rig it would apply.
A client that edits and asks again makes a new request for the new rig, and its
earlier one for the same sandbox expires, so two prompts that read the same are never
both waiting.

### Allowed folders

A command server client may only name files inside: the library root, the local model
stash, the scratch folder, and folders the user added (`roots.add`, local; `roots.list`
shows them all). Path arguments are marked in the command spec (`path: true` in
`commands.describe`) and checked by the hub before the command runs: absolute, and
inside one of the roots after `..` and symbolic links are resolved on both. This covers
`chain.addToneFromFiles`, `render.clip`'s `clips`, `render.compare`'s `reference`,
`guitar.importClip`, `preset.importJson`, `preset.exportJson` (the file it writes) and
the plugin folder commands.

- **Inside means inside.** A root itself is not inside it: a client cannot name the
  library folder as the file to write, or as the folder to scan.
- **Folders beside the file.** `preset.exportJson` writes model files into an `Assets`
  folder beside its target and `preset.importJson` reads them from there, so that
  folder is held to the roots too, where it resolves to: an `Assets` that is a link out
  of the allowed folders refuses the command. The export itself never writes through
  an `Assets` that is a link, wherever it points and whoever asks.
- **`plugins.scan`'s `files`** are absolute paths inside the roots with no `..` in
  them. A relative entry, one that climbs, and (for a client) an Audio Unit identifier
  are refused; the local caller may still name Audio Units.
- **The scratch folder** is `<app data>/Pedaleira/Scratch`: per user, made with mode
  0700 on macOS and Linux. It used to be a name in the system's temp directory, which
  another account could have made first. If the path is a symbolic link it is not
  used: it is not an allowed folder, and a render asked to write its audio there
  fails with the reason.

### Clients and activity

`agents.list` shows each connection with the name it declared, when it connected, its
last command and the rig it is on; `agents.disconnect` closes one. `agents.activity`
returns the last 200 commands that came through the hub: client, command, arguments
shortened to one line, the rig, and how it ended.

## Sandbox rigs

Offline work must never disturb the rig being played, so it happens in a sandbox: a
whole second processor with the engine's command set over it, never connected to an
audio device (`SandboxRig`). A second processor rather than a mode of the live one,
because nothing less keeps the live audio and state bit-identical while an agent tears
into a rig: the sandbox has its own blocks, models, parameters, scenes and undo history.
It shares only what is global anyway: the library (as a guest, see "Sandbox rigs and
the library"), the model stash, the plugin scanner.

The selection is per connection, not per command:

- `sandbox.create {from: live | empty | preset}` makes one and selects it for the
  calling connection. From then on every rig command of that connection (`chain.*`,
  `block.*`, `scene.*`, `param.*`, `preset.*`, `state.*`, ...) acts on the sandbox.
  A copy of the live rig loads its blocks in the background, so `state.waitUntilLoaded`
  comes before editing.
- `sandbox.use {sandbox}` switches between the connection's sandboxes and `live`.
- `sandbox.applyToLive` copies the sandbox onto the live rig the way loading a preset
  would (the chain with its scenes, and the preset-managed parameters), as one undo step
  there. This is the gated step; while it waits for approval, what it will apply is
  the sandbox as it was when asked (see "Approvals").
- `sandbox.discard` frees it. A connection's sandboxes go when it closes; one that a
  render is running through at that moment goes when the render ends. At most four
  exist at a time.

A sandbox belongs to the session that made it. Only that session can select it, edit
it, render through it, apply it or discard it, and `sandbox.list` shows a session its
own. To any other client the id answers `not_found`, the same as an id that does not
exist. The local session, which is the user, sees and can use them all. The private
copy a render of the live rig makes for itself cannot be addressed by anyone.

Why a selection and not a `rig` argument on every command: the rig commands stay
exactly as they are, a client written for the live rig works on a sandbox unchanged, and
an agent cannot forget the argument on one call and edit what the user is playing. The
cost is state on the connection, which the answers always name (`rig` in `sandbox.list`,
`events.subscribe` and the render results).

Machine-wide settings (`settings` class) always go to the live side, whatever is
selected. The rig commands of a sandbox are exactly those of `EngineCommands` in `rig`
scope, a test holds the two lists together.

What `applyToLive` does not carry: session settings that belong to the user's setup
rather than to the tone (input mode, oversampling, the MIDI map, multi-core). The live
rig takes the sandbox's active preset as its own, marked unsaved when the sandbox had
been edited since it loaded or saved that preset; the library is neither read nor
written (no recents, no position).

### Sandbox rigs and the library

The setlist library (`docs/library.md`) is the user's, and a sandbox is a guest in it
(`TONE3000Processor::sharePresetStoreWith`). The rule: **a sandbox only ever adds to the
library, and selecting one is never a way around a gate.**

- A sandbox's command set holds the library commands that have a rig in them: the reads
  (`preset.list`, `setlist.list`, `setlist.get`, `library.get`, `recent.list`), the loads
  (`preset.load`, `preset.loadSlot`), `setlist.select` and the three that write the rig
  out (`preset.save`, `preset.saveToSlot`, `preset.exportJson`).
- From a sandbox, a save goes into an empty slot: saving under the name of a preset of
  the setlist, or into a slot that holds one, fails (`failed`) instead of replacing it.
  An export fails when the file is already there. To change an existing preset, apply
  the sandbox to the live rig and save from there, which is the gated path.
- A sandbox's position (its current setlist and slot) and its loads stay in memory:
  `library.json` keeps the player's position and recents.
- Everything that only reorganises the library (`setlist.create/rename/delete/reorder`,
  `preset.rename/move/delete`, `preset.moveSlot/duplicateSlot/deleteSlot/copyToSetlist`,
  `preset.importJson`, `favourite.set`, `library.rescan`, `library.importLegacyPresets`)
  is not in a sandbox's command set. With a sandbox selected such a command runs on the
  live side through the same gate as with the live rig selected: an `edit` waits for
  approval while `live` is off, and what goes to the trash needs `delete`.

## Listen tools

An agent cannot hear, so these play audio through a rig offline and answer with
measurements. They never run through the live rig: the rig is the connection's selected
sandbox, the one named by `sandbox` (the caller's own), or a private copy of the live
rig made for the call and discarded after it. One render runs through a sandbox at a
time and it cannot be edited meanwhile (`unavailable`).

What a render may cost is bounded, because every render holds its clip, its output and
the measurement buffers in memory:

| Limit | Value | Past it |
|---|---|---|
| Renders per call (clips times scenes) | 32 | `invalid_args` |
| Audio rendered per call, summed over its renders (each clip with its tail, once per scene) | 600 s | `invalid_args`, from the files' headers, before anything is rendered |
| Renders running at once, over all connections | 2 | `unavailable`: try again |
| A clip or a reference | 120 s, mono or stereo, 8 to 192 kHz | `failed` |

A WAV file's header is checked before a buffer is made for what it claims: channel
count, sample rate, length, and the claimed length against the file's real size (a
header that names more audio than the file holds is refused; this matters for RF64
files, whose length is in a chunk of its own). Each render's audio is dropped once it
is measured; only `render.compare` keeps it, to join the takes, and gives each back as
it is copied.

A render prepares the rig, waits for its loads, prepares again (the sequence
`PedaleiraRender` uses), then runs 128-sample blocks on the calling thread, with the
reported latency rendered extra and dropped so the output lines up with the input.
Preparing clears every block's history, which is what makes the same call return the same
numbers. A mono clip is rendered through a one-input, two-output layout.

- `render.clip`: a guitar's takes (or WAV files inside the allowed folders), optionally
  once per scene, measured; `writeAudio` also writes each render to the scratch folder.
- `render.compare`: the same measurements for a reference recording and for the rig's
  render (all takes joined), with differences and a distance per group.
- `render.probe`: `response` (gain per frequency from a noise burst), `levelCurve`
  (output level for a stepped sine, -60 to 0 dBFS) and `noise` (silence in).

### Measurements

Defined in `AudioMeasure`. Levels, spectrum and stereo are measured over the stretch the
input played; the tail is measured as a tail.

| Reading | Definition | Simplifications |
|---|---|---|
| `peakDb` | Highest sample magnitude, dBFS. | |
| `truePeakDb` | The same after 4x interpolation (12 taps per phase, windowed sinc). | Not the BS.1770 annex filter; within about 0.2 dB for audio-band content. |
| `rmsDb` | RMS over all samples of all channels, dBFS. | A full-scale sine reads -3.01. |
| `crestDb` | `peakDb - rmsDb`. | |
| `lufs` | Integrated loudness, ITU-R BS.1770: K-weighting (shelf and 38 Hz high-pass, derived per sample rate), 400 ms blocks every 100 ms, absolute gate at -70 LUFS, relative gate 10 LU under the gated mean. | Left and right weigh 1 and add, so a mono clip on both outputs reads 3 dB over the clip itself; no surround weights; a signal shorter than 400 ms is one block. |
| `gainDb` | Output `rmsDb` minus input `rmsDb`. | By RMS, so a unity rig reads 0 whatever the channel counts. |
| `spectrum.bandsDb`, `bandShare` | Share of the energy between 20 Hz and 20 kHz in each band: sub 20 to 80, bass 80 to 250, lowMid 250 to 500, mid 500 to 2k, highMid 2k to 4k, presence 4k to 8k, air 8k to 20k. Welch average, 8192-point Hann segments, channel powers averaged. | Bin resolution about 6 Hz at 48 kHz: the sub band's lower edge is soft. |
| `centroidHz`, `lowEndShare` | Energy-weighted mean frequency; share below 250 Hz. | |
| `stereo.correlation` | Normalised left/right correlation, -1 to 1. | 0 when one side is silent. |
| `stereo.width` | sqrt(side energy / (mid + side energy)): 0 mono, 0.71 two unrelated channels, 1 out of phase. | |
| `stereo.balanceDb` | Right over left energy. | |
| `distortion.percent` | 100 x sqrt(residual energy / output energy), where the residual is what the best linear filter of the input does not explain. | See below. |
| `tail.seconds` | From the clip's end to the last 10 ms window within 60 dB of the output's level while the clip played. `truncated` when the render ended first. | Even an empty rig reads a few tens of milliseconds (its DC and tone filters ring on). |
| `latencySamples`, `latencyMs` | The rig's reported latency, which the render compensated. | Reported, not measured. |
| `dynamics` | For a position with a soft and a hard take: both distortion readings, `cleanupRatio` (soft over hard), the level difference in and out, and `compressionDb` (how much of it the rig removed). | Level differences in LUFS. |

The distortion estimate: the output is divided by the input bin by bin wherever the
input has energy (within 40 dB of its strongest bin; gaps are bridged in a straight
line), and the resulting filter's impulse response is cut to a short span: an eighth of
the rendered length after time zero, at most a second, and a quarter of that before.
What that filter predicts is the linear part; the rest is the residue. Consequences
worth knowing:

- A clean rig reads near 0, a clipped sine reads its true harmonic share, and more drive
  reads as more.
- Delay and reverb that fit inside the span are modelled; later echoes, long tails and
  any modulation (chorus, a compressor riding the gain) read as residue. To measure an
  amp's own distortion, bypass the time effects.
- On a perfectly steady note, harmonics the rig adds land on harmonics the note already
  has, where a filter can account for them; playing with dynamics (any real take) does
  not have this problem, but readings are lower than a THD meter's on a sine would
  suggest. Compare like with like: the same takes, rig against rig.
- Only the first 20 seconds of a render are fitted.

`render.compare` distances are scaled so that 1 is a difference a listener would not
miss: loudness |delta LUFS| / 3, dynamics |delta crest| / 3, tone the larger of (RMS of
the band differences) / 3 dB and |centroid difference| / a third of an octave, stereo
the larger of |delta correlation| / 0.3 and |delta width| / 0.2. Overall is 0.5 tone +
0.2 loudness + 0.2 dynamics + 0.1 stereo. The scales are a first guess, to be tuned by
listening.

`render.probe response` reads the fitted filter's gain in twelfth-octave bands around
each frequency, with `coherence` (how much of the output there the filter explains):
near 1 where the rig is linear at that level, lower where it is not, and then the gain
means less. `levelCurve` reports sine peak levels, so unity is 0 dB of gain.

## Guitar profiles

A profile is what one guitar puts into the rig: DI clips per pickup position, each a
take (`soft`, `hard`, `chords` or `notes`), with measurements. Under the library root:

```
Guitars/guitars.json                 { "default": "<id>" }
Guitars/<id>/profile.json            name, pickupType, positions, tuning, notes, takes, summary
Guitars/<id>/<position>-<take>.wav
```

The library root is resolved in one function, `LibraryPaths::libraryRoot()`, as
`<Documents>/Pedaleira`: the setlist library's root (`SetlistLibrary::defaultRoot()` is
that function), so `Guitars` sits beside `Setlists` and `Assets`
(`docs/library.md`, "Folder layout"). The agent layer asks the live rig for the root of
the library it uses, which is that folder unless a test gave the processor its own. A
deleted profile goes to the trash the way a deleted preset does.

Per take: `peakDb`, `rmsDb`, `lufs`, `crestDb`, `centroidHz`, the band balance,
`lowEndShare`, `hum` (the 50 and 60 Hz components, which of them stands out as mains,
and its level with harmonics) and `noiseFloorDb` (the quietest tenth of the clip's 50 ms
windows, only when the clip has a pause at least 30 dB under its level; start a take
with a moment of silence). The summary bins the numbers: `outputLevel` low / medium /
high by the loudest peak (-18 and -6 dBFS), `brightness` dark / balanced / bright by
the mean centroid (700 and 1400 Hz), `noise` quiet / moderate / noisy by floor and hum.
The bins are rough and depend on the interface gain the clips were recorded with.

Commands: `guitar.list`, `guitar.get`, `guitar.create`, `guitar.update`,
`guitar.delete`, `guitar.setDefault`, `guitar.importClip` (attach an existing WAV),
`guitar.analyze` (measure again), and for the application `guitar.record.start`,
`guitar.record.status`, `guitar.record.cancel`.

Recording is the one place this layer touches the audio thread. `InputRecorder::capture`
runs first thing in `processBlock`: one atomic read while idle; while recording, a copy
of one input channel into a buffer allocated beforehand, no lock, no allocation. The
first `guitar.record.status` after the buffer is full hands the samples to a writer
thread, which saves the WAV, measures it and adds the take. Recording listens to what
the user plays, so for a client it is a `live` command.

## Discovery

- `kinds.list`: every block kind with its parameters.
- `plugins.list`: scanned plugins.
- `guitar.list`, `guitar.get`.
- `assets.list`: `.nam` captures and `.wav` IRs under the library root and the added
  folders (tags are the folder names), in the library's asset store (the model files of
  the saved presets) and in the model stash, with what a capture file
  says about itself (`nam_name`, `nam_gear_type`, `nam_gear_make`, `nam_gear_model`,
  `nam_tone_type`, `nam_gain`, `nam_loudness`) and an IR's rate, channels and length;
  plus `inRig`, the tones the rig already holds with their titles and gear tags, and the
  user's own tags (below).

### Finding captures by name

The model stash and the asset store keep a loaded file under a content-hash name, so the
engine remembers the name a file was loaded under (`NamLibrary`, kept in
`<app data>/NamRecents.json`, outside the stash because the stash drops what a week
has not used).

- `assets.list` lists such a copy by that name (`name`, `loadedFrom`, `loadedAt`), falls back to the
  name in the file's metadata, and a `query` finds it by either. `origin` says where a
  file came from: `library`, `folder` (added with `roots.add`), `tone3000` (downloaded
  on Pedaleira's TONE3000 page, kept in `<library>/TONE3000`, listed with its `tone3000`
  credit) or `loaded`.
- `nam.recent` lists the captures loaded from a file, newest first, each with the path to load
  again (the original, else the stash copy); `nam.clearRecent` (local) empties it.
- `nam.downloaded {tone, capture}` returns the file a TONE3000 capture was downloaded to
  before, if it is still there, so it is not downloaded twice; `nam.keepDownload` (local)
  moves a fresh download into `<library>/TONE3000` and records its credit. The TONE3000
  desktop app has no downloads folder, so none is looked for.
- The architecture check: `assets.list` gives each capture `architecture`,
  `architectureLabel`, `a2`, `sampleRate` (when the file names it) and `supported`
  (every architecture NAM builds loads, A1 WaveNets, LSTMs, ConvNets and Linear models
  included, each run at the rate it was trained at), with `unsupportedReason` in a
  sentence for a file that is not a capture, an unknown architecture or version, or
  damaged; `nam.inspect {path}` answers for one file; loading an
  unsupported file (`chain.addToneFromFiles`) fails with the same sentence.

### Captures and IR index

The user's own tags on captures and IRs, for the "Tone tagging" card in Settings and for
an agent choosing a tone ("bright", "edge of breakup", "v30").

- Kept in `<library>/asset-tags.json` (`AssetTags`), keyed by each file's SHA-256, so a
  tag follows the file through a move, a rename or a copy into the asset store (whose
  names are that hash already). A path's hash is remembered with its size and
  modification time and computed again only when either changes.
- Tags are lowercased, trimmed, spaces collapsed, at most 32 characters, at most 32 per
  file.
- `assets.tag {file, add, remove}` (`edit`) changes a file's tags.
- `assets.list` gives every asset `tags` (folder tags and the user's together),
  `userTags` and `tagged`; `tags: [...]` keeps only assets carrying all of them and
  `untagged: true` only those the user has not tagged (the "Review untagged items" list).
  A `query` also matches the user's tags.
- `assets.tagSummary`: captures and IRs, total and tagged; every tag in use with its
  count; a suggested vocabulary (gain, character, gear, speaker, use) to keep tags
  consistent. A suggestion only: any tag is accepted.
- The MCP tool `tone_describe_capabilities` takes `assetTags` and hands the user's tags
  over as untrusted data, like names and folder tags.

Still missing: tones from the online catalogue are only known once they are in a rig;
stash and asset store files have content-hash names and no titles (a preset names its
assets, see `docs/library.md`, but nothing lists them by that name yet); nothing measures
a capture or classifies an untagged IR as cab or room, so the tags are only what the user
gives.

## Tone requests

"Describe a tone" (the canvas's Tone request, working, result and refine boards) runs
through the agent the user has connected over MCP. The application holds one request at a
time; the agent serves it with the same tools it builds any tone with.

| Status | Meaning |
|---|---|
| `waiting` | Made by the user, no agent working on it (or the agent that was disconnected). |
| `working` | Claimed by an agent, which reports its steps; also after a refine message. |
| `ready` | The agent proposed a tone: its sandbox, a title, a summary, what changed, what it measured. |
| `applied` | The user applied it to the rig being played. |
| `stopped` | The user stopped it; what was built stays in the agent's sandbox. |

The user's side is local (the UI): `toneRequest.create {prompt, guitar?, reference?,
limits?, target}`, `toneRequest.reply {text}` (refine), `toneRequest.stop` and
`toneRequest.apply`, which applies the proposal's sandbox to the live rig as the user, so
no approval prompt follows (it is the user's own click). `AgentHub::onToneRequestChanged`
fires on every change and `toneRequest.get` (read) returns the request.

The agent's side:

1. `toneRequest.wait {since, timeoutMs}` (read) returns when the request changed after
   `since`, or after the wait (at most 25 s) with the same sequence. An agent that serves
   requests calls it in a loop.
2. `toneRequest.claim {id}` (edit): this connection works on it; another is refused.
3. Build in its own sandbox (`sandbox.create`, the rig commands, `render.*`), reporting
   `toneRequest.progress {id, text, step?, of?}` (edit) as it goes.
4. `toneRequest.propose {id, sandbox, summary, title?, changes?, measured?}` (edit): the
   sandbox must be one this connection may address. The summary is also the agent's
   answer in the conversation.
5. A refine message from the user shows as `unansweredMessages` on the next wait; the
   agent works again and proposes again.

Bounded: a prompt is at most 4000 characters, a message or step 2000, 200 steps (the
oldest go), 100 messages, 64 changes. When the claiming connection goes, the request
returns to `waiting` and its proposal is dropped (the sandbox went with the session).

## What the UI will need

- **Settings page for agents.** The server switch (`server.setEnabled`, `server.status`
  with the connection file path), the six permission switches (`permissions.get/set`),
  the allowed folders (`roots.list/add/remove`), the connected clients with a disconnect
  button (`agents.list`, `agents.disconnect`) and the activity log (`agents.activity`).
- **Approval prompt.** `AgentHub::onApprovalsChanged` fires when a request starts or
  stops waiting; `approvals.list` has who wants what; `approvals.resolve` answers. For
  `sandbox.applyToLive` the request carries `applies` (the sandbox, the revision it was
  captured at and `sandboxChanged`). The prompt can offer a preview by rendering through
  that sandbox (`render.clip {sandbox}` as the local session) while `sandboxChanged` is
  false; once it is true the sandbox is no longer what an approval would apply, and the
  prompt should say so instead of previewing it.
- **Add a guitar.** A wizard over `guitar.create`, then per position and take
  `guitar.record.start` / `guitar.record.status` (progress and input peak for a level
  display) / `guitar.record.cancel`, then the profile's summary. It should ask for a
  moment of silence before playing and pick the input channel.
- **Describe a tone.** The request form (`toneRequest.create`), the working view (steps,
  step of total, Stop), the result (title, summary, measured, Audition by rendering the
  proposal's sandbox, Apply) and the refine chat with its changes list
  (`toneRequest.reply`, `changes` before and after).
- **Tone tagging.** The counts from `assets.tagSummary`; "Review untagged items" lists
  `assets.list {untagged: true}` and tags with `assets.tag`, offering the vocabulary.
- **A marker for sandbox work**, if the UI ever shows a client's sandbox: `sandbox.list`.

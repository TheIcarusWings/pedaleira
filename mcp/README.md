# Pedaleira MCP server

A stdio MCP server that lets an agent drive Pedaleira: every engine command as a tool,
plus a handful of tone tools for the loop that matters: build a rig in a sandbox, play
the user's own recorded guitar clips through it offline, read the measurements, adjust,
and ask for the result to be put on the rig being played.

It is a thin sidecar. It keeps no state of its own and knows nothing about any
particular command: the tool list is generated from the engine's `commands.describe`.
The engine side is described in [`docs/agents.md`](../docs/agents.md).

## Setup

Requires Node 22 or newer.

```
cd mcp
npm ci
npm run build
```

Then switch Pedaleira's command server on (it is off by default) or, without the
application, run the headless host:

```
build/tools/PedaleiraHeadless_artefacts/Release/PedaleiraHeadless --data ~/pedaleira-agent
```

It prints the path of the connection file. With the application, the file is at
`~/Library/Application Support/Pedaleira/command-server.json` (macOS),
`%APPDATA%\Pedaleira\command-server.json` (Windows) or
`~/.config/Pedaleira/command-server.json` (Linux), which is where this server looks by
default.

### MCP client configuration

Any MCP client that starts stdio servers takes a command and an environment. The usual
shape:

```json
{
  "mcpServers": {
    "pedaleira": {
      "command": "node",
      "args": ["/absolute/path/to/pedaleira/mcp/dist/src/index.js"],
      "env": {
        "PEDALEIRA_CLIENT_NAME": "Tone agent",
        "PEDALEIRA_CONNECTION_FILE": "/Users/you/pedaleira-agent/command-server.json"
      }
    }
  }
}
```

| Variable | Meaning | Default |
|---|---|---|
| `PEDALEIRA_CONNECTION_FILE` | Where the port and the token are read from. | The application's own location (above). |
| `PEDALEIRA_CLIENT_NAME` | The name shown in Pedaleira's list of connected clients and on approval requests. | `MCP client` |

The server connects on the first tool call and again whenever the connection has gone:
Pedaleira restarting gives it a new port and token, which are read afresh from the file.
While Pedaleira is not reachable, the tone tools are still listed and answer with
`not_connected` and what to do about it. The connection file names the process that
wrote it (`pid`); when that process has gone (a crash leaves the file behind) the
answer is "Pedaleira is not running" and the token is not sent to whatever has the port
now. The check is best effort: a process id that has been given out again looks alive,
and the connection then fails on the port or the token instead.

## Tools

### Tone tools

| Tool | What it does |
|---|---|
| `tone_describe_capabilities` | The block kinds by category (with parameters on request), the captures and IRs in the library with their tags, the tones already in the rig, the guitar profiles, usable plugins and this client's permissions. |
| `tone_start` | Creates a sandbox rig (from the live rig, empty, or from a preset) and selects it: from then on every rig tool edits the sandbox, not what the user is playing. |
| `tone_render` | Plays a guitar profile's clips through the sandbox offline and returns the measurement summary per take: loudness, peak, crest, gain, distortion percent, spectral centroid and band balance, stereo width, tail; plus how the rig treats soft against hard picking. With `reference`, also the distance from a reference recording. |
| `tone_apply` | Asks for the sandbox to be put on the live rig. Answers `needs_approval` with an `approvalId` unless the user has allowed live changes; called again with that id (and `waitSeconds`), it reports the outcome. |
| `tone_discard` | Throws the sandbox away and returns to the live rig. |

### Untrusted text

Much of what the tools return was written by somebody: preset, tone, block, guitar and
plugin names, guitar notes and tunings, pickup position names, folder tags, file paths,
and the `nam_*` fields a capture file carries about itself (a file the user may have
downloaded from anywhere). Any of it can hold a sentence aimed at the model. None of
it is an instruction:

- The server's `instructions` say so, and so does the description of every tool whose
  result can hold such text (all command tools, and the tone tools that return names).
- The tone tools put these fields under one key, `untrusted`, apart from what the
  engine vouches for (ids, kinds, numbers), and add an `untrustedNote` to the result:

  ```json
  {
    "kind": "capture",
    "path": "/Users/you/Documents/Pedaleira/Captures/Amps/crunch.nam",
    "source": "library",
    "untrusted": { "name": "crunch", "tags": ["Captures", "Amps"], "nam_name": "JCM800 crunch", "nam_gear_make": "Marshall" }
  }
  ```

  Captures and IRs: `name`, `tags`, `nam_*`. Tones in the rig: `title`, `gear`. Guitars:
  `name`, `tuning`, `positions`, `notes`. Plugins: `name`, `vendor`. Blocks: `name`.
  Rig summaries: `preset`. Renders and dynamics: `position`.
- The command tools and the `pedaleira://rig/state` resource return the engine's answer
  as it is, with such fields in place; the same rule holds there.

This labels the data; it does not make a model immune to it. What limits the damage is
on Pedaleira's side: the permission switches, the approval the user gives for anything
that changes the rig being played, and the allowed folders.

### Command tools

One per engine command, named with underscores for dots: `chain.addBlock` is
`chain_addBlock`, `guitar.record.start` is `guitar_record_start`. Input schemas come
from the command's parameter specs (types, enums, ranges, defaults); descriptions are
the command summaries with the permission class and the untrusted-text rule appended.
At the time of writing that is 110 tools; commands of the `local` class are not offered, since the engine never runs
them for a client.

### Resources

| URI | Content |
|---|---|
| `pedaleira://commands` | The command reference as markdown. |
| `pedaleira://rig/state` | The full state of the rig the connection is working on (the sandbox after `tone_start`, else the live rig), as JSON. |

### Errors

An engine error comes back as a tool error whose text is `<code>: <message>` with the
engine's own code and message, and the same in `structuredContent.error`. Codes:
`invalid_args`, `not_found`, `failed`, `unavailable`, `unknown_command`,
`permission_denied`, `needs_approval` (with `approvalId`), and `not_connected` from this
server when Pedaleira cannot be reached.

## Permissions

Pedaleira decides what a client may do; this server only passes requests on. The user has
six switches:

| Switch | Default | With it off |
|---|---|---|
| Read library, presets and rig state | on | Every read is refused. |
| Create and edit presets and rigs | on | No sandbox can be made; nothing can be edited. |
| Render audio clips to measure a tone | on | `tone_render` and the `render_*` tools are refused. |
| Change the rig being played | **off** | Edits aimed at the live rig, and `tone_apply`, answer `needs_approval` and wait for the user. Edits in a sandbox need nothing. Reorganising the library (`setlist_create`, `preset_rename`, `preset_moveSlot`, `preset_importJson`, `favourite_set`, ...) waits for the user whichever rig is selected; from a sandbox `preset_save` only adds a preset, it never replaces one. |
| Delete presets, setlists and guitar profiles | **off** | `preset_delete`, `preset_deleteSlot`, `setlist_delete`, `guitar_delete` are refused. |
| Audio, device and machine-wide settings | **off** | Footswitch mode and plugin scanning are refused. |

Files: a tool may only name files inside the library, the model stash, the scratch
folder and folders the user has added (`roots_list` shows them). Anything else is
refused with `permission_denied`.

An approval request shows the user this client's name, the command and its arguments.
It expires after ten minutes. `tone_apply` asks about the sandbox as it is at that
moment, and that is what an approval applies: edit first, ask after. Asking again after
an edit makes a new request and withdraws the earlier one.

A sandbox belongs to the connection that made it; another client can neither see nor
use it. One call renders at most 600 seconds of audio (clips and tails, once per
scene), and two renders run at a time in the whole application: a third answers
`unavailable` and is worth trying again.

## Worked example: a crunch tone for one guitar

What an agent does when the user says "give me a crunchy rhythm tone that cleans up when
I play softly, on my Les Paul". Results are abridged.

1. Find out what there is.

   ```
   tone_describe_capabilities {}
   -> guitars: [{id: "les-paul", positions: ["bridge","neck"], takeCount: 4,
                 summary: {outputLevel: "high", brightness: "balanced", noise: "quiet"}}],
      captures: [{name: "Plexi crunch", path: ".../Amps/Plexi crunch.nam", tags: ["Amps"],
                  nam_gain: 0.55}, ...],
      irs: [{name: "4x12 V30", path: ".../Cabs/4x12 V30.wav"}, ...],
      kinds: [{id: "dynamics.compressor", category: "dynamics", params: [...]}, ...],
      permissions: {read: true, edit: true, render: true, live: false, ...}
   ```

2. Start a sandbox and build in it. Nothing the user hears changes.

   ```
   tone_start {from: "empty", name: "LP crunch"}          -> {sandbox: "sandbox-1", blocks: []}
   chain_addToneFromFiles {paths: [".../Amps/Plexi crunch.nam"]}   -> {blockId: "amp1", ...}
   chain_addToneFromFiles {paths: [".../Cabs/4x12 V30.wav"]}       -> {blockId: "cab1", ...}
   chain_addBlock {kind: "reverb.room"}                            -> {blockId: "verb1", ...}
   ```

3. Render the guitar's own clips and read the result.

   ```
   tone_render {guitar: "les-paul", position: "bridge"}
   -> renders: [
        {take: "soft", lufs: -19.8, distortionPercent: 21.0, centroidHz: 1450, ...},
        {take: "hard", lufs: -15.1, distortionPercent: 38.5, centroidHz: 1820, ...}],
      dynamics: [{position: "bridge", softDistortionPercent: 21.0, hardDistortionPercent: 38.5,
                  cleanupRatio: 0.545, compressionDb: 9.2}]
   ```

   The soft take still reads 21 percent: it does not clean up enough.

4. Adjust and render again. The numbers are repeatable, so a change in them is the
   change that was made.

   ```
   block_setParam {blockId: "amp1", param: "inputGain", value: 0.38}
   tone_render {guitar: "les-paul", position: "bridge"}
   -> dynamics: [{softDistortionPercent: 9.5, hardDistortionPercent: 31.0,
                  cleanupRatio: 0.306, compressionDb: 6.1}]
   ```

   For the tone's balance, `spectrum.bandsDb` says where the energy is; for a target
   recording the user supplied, `tone_render {reference: ".../target.wav"}` adds a
   distance per group (loudness, dynamics, tone, stereo) to steer by. `render_probe`
   measures a rig without a clip: the frequency response of an EQ, the level curve of a
   compressor, the noise floor.

5. Ask for it to go live. The user sees a prompt naming this client and approves.

   ```
   tone_apply {}                                   -> {status: "needs_approval", approvalId: "approval-3"}
   tone_apply {approvalId: "approval-3", waitSeconds: 60}   -> {status: "applied", applied: "sandbox-1"}
   preset_save {name: "LP crunch"}                 (in the sandbox: saves it for later)
   tone_discard {}
   ```

   On the live rig this is one undo step.

Notes for whoever writes the agent's instructions:

- Distortion readings are comparable between renders of the same takes, not absolute:
  delay, reverb and modulation count as "not linear" too, so judge an amp's distortion
  with those bypassed (`block_setEnabled`), and see `docs/agents.md` for what each
  measurement is and leaves out.
- A sandbox made from the live rig loads its blocks in the background; `tone_start`
  waits for that, `sandbox_create` does not (`state_waitUntilLoaded`).
- One render runs through a sandbox at a time, and it cannot be edited meanwhile.

## Development

```
npm test
```

builds and runs the tests against a real engine: each test spawns `PedaleiraHeadless`
(build it first: `cmake --build build --target PedaleiraHeadless` in the repository root,
or point `PEDALEIRA_HEADLESS` at the binary) with scratch folders, and approves requests
through the host's standard input, which stands in for the user.

Dependencies are pinned: the MCP TypeScript SDK and zod (which the SDK requires) at run
time; TypeScript and Node's type definitions to build. The test runner is Node's own.

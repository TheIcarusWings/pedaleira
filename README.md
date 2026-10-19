# Pedaleira

Pedaleira is an open-source desktop guitar rig. You build a tone on a grid of rows,
each row a chain of blocks: Neural Amp Modeler (NAM) captures, impulse responses (IRs),
built-in effects and hosted VST3/AU plugins. Rows can split and mix, scenes and
footswitch modes change the rig while you play, and presets live as plain JSON files in
setlist folders. The workflow is inspired by hardware modelers.

It is built for macOS first, then Windows and Linux. It runs as a standalone app first,
and VST3 and AU plugins come from the same code. It can also be driven by an AI agent:
a command layer and an MCP server let an agent build a tone and measure it against
recordings of your own guitar.

Pedaleira is a fork of [tone-3000/tone3000-plugin](https://github.com/tone-3000/tone3000-plugin)
(JUCE, C++20). It keeps the upstream NAM and IR engine and its TONE3000 loading. It is
not affiliated with any hardware modeler maker, and it copies nothing of their look,
code or models.

## Status

Work in progress, not released. The engine is built and tested, and the standalone app now
opens Pedaleira's own window: the grid editor, the block editors, scenes and footswitches,
Gig View, the library, the preset importer and the capture wizard are in, as first passes.
Settings and the TONE3000 page are next. The plugin formats still show upstream's screen.
There is no installer and no release yet.

## Features

Built today (engine side, reachable through commands, the headless renderer and the
MCP server):

- NAM captures (A2 architecture) and IRs, loaded from local files or from TONE3000.
- A grid of rows with splits, mixers, row inputs and outputs, next to upstream's two-lane
  chain.
- 26 built-in effects: dynamics, EQ and filter, delay, reverb, modulation and pitch
  ([docs/effects.md](docs/effects.md)). They are verified by measurement; nobody has
  listened to them yet.
- Hosted VST3 and AU plugins, scanned in a helper process, each opening its own editor window.
- Scenes A to H, stomps, footswitch modes and tap tempo ([docs/scenes.md](docs/scenes.md)).
- A setlist library of JSON presets with favourites and recents
  ([docs/library.md](docs/library.md)).
- A command layer for every state change, with state as JSON
  ([docs/commands.md](docs/commands.md)).
- A headless renderer, with golden render tests.
- A local command server with permissions and approvals, sandbox rigs, render and
  measure tools, guitar profiles, and an MCP server ([docs/agents.md](docs/agents.md)).
- Tags on your captures and IRs, and tone requests: you describe a tone in the app and
  your connected agent builds it, reports its steps and takes refine messages.
- Gain-reduction readings on the dynamics effects, and a WebSocket transport for the
  coming remote.
- A preset importer with one reader per source, Quad Cortex first: a read-only reader on
  pyquadcortex and a mapping table not yet checked against a real unit
  ([docs/import.md](docs/import.md)).
- An effect-matching kit: a test signal, an analysis of recordings of it and a compare,
  for tuning the built-in effects by measurement ([docs/matching.md](docs/matching.md)).

Planned:

- The UI (gated on design sign-off), plugin editor windows, and standalone device
  channels for multi-interface setups.
- A capture wizard that trains NAM A2 captures from your own gear.
- A phone remote.
- Windows and Linux releases, after the Mac version is in daily use.

## Test builds

The "Pedaleira test build" workflow (Actions tab, Run workflow) builds the
app, the VST3 and, on macOS, the AU, and attaches them to the run as
`Pedaleira-macos-arm64-buildN`, `Pedaleira-windows-x64-buildN` and
`Pedaleira-linux-x64-buildN`, N being the run number. The app shows the same
number and the date in its window title and in Settings > About. They are for
testing: not signed or notarised.

- macOS (Apple silicon): unzip, move `Pedaleira.app` to Applications, then
  right-click it and choose Open the first time (or run
  `xattr -dr com.apple.quarantine /Applications/Pedaleira.app`). Plugins go to
  `~/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/Components`.
- Windows: keep `Pedaleira.exe` next to `PedaleiraPluginScan.exe`. SmartScreen
  warns about an unknown publisher: More info, then Run anyway. The VST3 goes
  to `C:\Program Files\Common Files\VST3`.

## Building

### Prerequisites

- [CMake](https://cmake.org/download/) 3.22 or newer, Git, and Ninja (optional but used
  below).
- A C++20 compiler: Xcode command line tools on macOS (full Xcode is only needed for the
  iOS presets), MSVC on Windows, GCC or Clang on Linux.
- JUCE and other dependencies are fetched by CMake at configure time. No manual install.
- Node 22 or newer, only for the MCP server.

### Get the submodules

```sh
git submodule update --init --recursive
```

### macOS: Standalone, AU and VST3

```sh
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_AAX=OFF -DBUILD_CLAP=OFF -DBUILD_LV2=OFF
cmake --build build
```

Artefacts land in `build/plugin/TONE3000_artefacts/Release/<format>/`. The CMake project
and target names are still upstream's; the product is named Pedaleira.
`./script/install-plugin.sh VST3` (or `AU`) copies a built plugin to the usual folders
(the install script is upstream's and has not been reviewed for Pedaleira).

### Linux: test build

Linux is built in CI to run the DSP tests and the tools. The plugin formats are not
built there.

```sh
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libgtk-3-dev libasound2-dev libjack-jackd2-dev \
  libcurl4-openssl-dev libfreetype6-dev libx11-dev \
  libxcomposite-dev libxcursor-dev libxi-dev libxinerama-dev \
  libxrandr-dev libxrender-dev xvfb

cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/linux-toolchain.cmake \
  -DBUILD_AAX=OFF -DBUILD_CLAP=OFF
cmake --build build --parallel --target DspTests PedaleiraRender PedaleiraPluginScan
ctest --test-dir build/test --output-on-failure --timeout 300
```

Windows and macOS run the same test target in CI on demand (see
[.github/workflows/pedaleira-ci.yml](.github/workflows/pedaleira-ci.yml)). If you switch
CMake presets, remove `build` and reconfigure. The run-time dependencies of the Linux
standalone are in [docs/upstream-audio.md](docs/upstream-audio.md).

Useful options: `-DHEADLESS=ON` for headless builds, `-DT3K_BUILD_UI_TESTBED=ON` for the
UI testbed ([plugin/ui/README.md](plugin/ui/README.md)). `./script/test-dsp.sh` builds
and runs the DSP tests (add a gtest filter as the first argument).

### TONE3000 publishable key

Builds carry Pedaleira's own publishable key (set in `plugin/ui/NativeUi.cmake`), so
sign-in to TONE3000 works with no setup. A publishable key is public by design: it is
the OAuth client_id and ships inside the app. To use another key, put it in a `.env` file
at the repo root (`.env.local` overrides it, and the configure environment overrides
both; [.env.example](.env.example) lists every variable), then reconfigure:

```sh
# .env
T3K_PUBLISHABLE_KEY=t3k_pub_your_key_here
```

Sign-in opens in the system browser and returns through a loopback redirect, which
TONE3000 allows for publishable keys. Pedaleira uses TONE3000's free API tier, which covers
open source projects: the sign-in flows and the bounded lists, no in-app search. Never put a
secret key in the repo or the app; Pedaleira does not use one.

## Tools

All are built by default, and are in `build/tools/<name>_artefacts/Release/`.

- **PedaleiraRender** runs a rig over a WAV through the real processor, with no window
  and no audio device. The rig comes from a preset (`.t3kpreset` or library JSON), a
  script of engine commands, or both. `--describe-markdown` prints the command
  reference.

  ```sh
  PedaleiraRender --preset p.t3kpreset --in in.wav --out out.wav
  ```

- **PedaleiraPluginScan** scans one plugin file in a process of its own, so a plugin
  that crashes or hangs while loading cannot take the app down. The engine runs it once
  per candidate.
- **PedaleiraHeadless** is the engine with its command server and nothing else. It is
  what the command server tests and the MCP server run against, and lets an agent work
  on a machine where the app is not open. Build it with
  `cmake --build build --target PedaleiraHeadless`.
- **PedaleiraMatch** writes the effect-matching test signal, analyses a recording of it
  and compares two analyses ([docs/matching.md](docs/matching.md)).
- **PresetTool** is a maintainer utility inherited from upstream for the shipped
  `.t3kpreset` files; `--import-foreign` converts a foreign preset and prints the report.
  It is not built by default.
- **tools/import/** holds the preset readers, the Quad Cortex one first
  ([tools/import/README.md](tools/import/README.md)).
- **MCP server** (`mcp/`) exposes every engine command as a tool, plus tone tools to
  build a rig in a sandbox, render a guitar profile's clips through it, read the
  measurements and ask for the result to go live. Setup, permissions and a worked
  example are in [mcp/README.md](mcp/README.md). The engine side is in
  [docs/agents.md](docs/agents.md).

## Documentation

| File | What it covers |
|---|---|
| [docs/agents.md](docs/agents.md) | Command server, permissions, sandbox rigs and the listen tools. |
| [docs/commands.md](docs/commands.md) | The command reference, generated from the registry. |
| [docs/effects.md](docs/effects.md) | How to write a built-in effect. |
| [docs/cab-framework.md](docs/cab-framework.md) | The cab model: a mic'd guitar cab computed from physics, and the speaker description format. |
| [docs/scenes.md](docs/scenes.md) | Scenes, stomps and footswitches. |
| [docs/library.md](docs/library.md) | The setlist library and its JSON preset format. |
| [docs/import.md](docs/import.md) | Importing presets from other products: the foreign format, readers, mapping tables. |
| [docs/matching.md](docs/matching.md) | Matching the built-in effects to a reference unit by measurement. |
| [docs/ios.md](docs/ios.md) | The iPad build, inherited from upstream. |
| [docs/upstream-audio.md](docs/upstream-audio.md) | The upstream signal path, settings stores, DSP tests and plugin validation the engine grew from. |
| [plugin/docs/](plugin/docs/) | Upstream design notes: local models, model latency, multi-core, native UI, oversampling, pitch shift, stereo image. |
| [plugin/ui/README.md](plugin/ui/README.md) | The JUCE UI code and testbed. |

## Repository layout

| Path | Contents |
|---|---|
| `plugin/` | The C++ engine and plugin: processor, DSP, effects, presets. Vendors NeuralAmpModelerCore and AudioDSPTools. |
| `plugin/ui/` | The upstream JUCE UI, and Pedaleira's own UI in `plugin/ui/pedaleira/`. |
| `plugin/docs/` | Upstream design notes. |
| `tools/` | PedaleiraRender, PedaleiraPluginScan, PedaleiraHeadless and PresetTool. |
| `mcp/` | The MCP server (TypeScript, Node 22). |
| `test/` | GoogleTest DSP suite, golden renders and test assets. |
| `docs/` | Pedaleira design and reference docs. |
| `cmake/` | CMake modules, including the Linux toolchain file. |
| `script/` | Build, packaging and install helpers, mostly upstream's. |
| `resources/` | Factory presets and other embedded resources. |
| `design/` | Figma exports and UI reference assets. |

## Licensing and credits

The code is under the **MIT License** ([LICENSE](LICENSE)). The copyright line there is
the upstream project's (TONE3000), and it must stay with any copy of this code. The plan
is to release Pedaleira as open source. JUCE has its own licensing (AGPLv3 or a
commercial licence, see [JUCE licensing](https://juce.com/legal/juce-9-licence/)), so a binary
release that includes JUCE is distributed under the AGPLv3 terms.
**NeuralAmpModelerCore** carries its own license in its directory. **AudioDSPTools**'
`ResamplingContainer` originates from the iPlug2 project (license in that source). The
CLAP build, which Pedaleira does not use, takes **clap-juce-extensions** and the **CLAP**
SDK (both MIT), fetched at configure time.

Pedaleira would not exist without these:

- [tone-3000/tone3000-plugin](https://github.com/tone-3000/tone3000-plugin): the upstream
  this project is forked from, and the source of the TONE3000 integration, the native UI
  and most of the audio engine.
- [TONE3000](https://www.tone3000.com): NAM captures and IRs. Use of its API follows its
  terms, including attribution and branding.
- [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler) by Steven
  Atkinson: the NAM ecosystem and
  [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore), which
  powers all amp modeling here.
- [NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin): the
  reference NAM plugin; the tone stack borrows its band voicing and the DC blocker
  matches its behavior.
- [AudioDSPTools](https://github.com/sdatkinson/AudioDSPTools): resampling around the
  48 kHz chain boundary, with `ResamplingContainer` from
  [iPlug2](https://github.com/iPlug2/iPlug2).
- [NAM-Oversampler](https://github.com/DLC86/NAM-Oversampler) by DLC86: oversampled NAM
  processing; the chain oversampler's half-band allpass coefficients are adapted from its
  AudioDSPTools fork (MIT). See
  [plugin/docs/oversampling.md](plugin/docs/oversampling.md).
- Pitch Shift started from a contribution by Vivek Radhakrishna
  ([#133](https://github.com/tone-3000/tone3000-plugin/pull/133)) built on
  [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch); the
  shipped engine is the correlation-spliced delay line documented in
  [plugin/docs/pitch-shift.md](plugin/docs/pitch-shift.md) (Eventide H949 de-glitch
  lineage; Juillerat et al.'s low-latency shifting papers and the Signalsmith write-up
  [Four Ways To Write A Pitch-Shifter](https://signalsmith-audio.co.uk/writing/2023/stretch-design/)
  were the references). The built-in pitch blocks reuse this engine.
- [JUCE](https://juce.com): plugin framework, DSP building blocks, and the UI toolkit.
- [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions): the CLAP
  wrapper.
- O. Das, ["An Open-Source Stereo Widening Plugin"](https://www.dafx.de/paper-archive/2024/papers/DAFx24_paper_92.pdf)
  (DAFx24): the allpass decorrelation approach used by the spread doubler.
- A. Farina, ["Simultaneous Measurement of Impulse Response and Distortion with a
  Swept-Sine Technique"](https://www.melaudia.net/zdoc/sweepSine.PDF) (108th AES
  Convention, 2000) and C. Knapp and G. Carter, "The Generalized Correlation Method for
  Estimation of Time Delay" (IEEE TASSP, 1976): the sweep probe and GCC-PHAT estimator
  behind auto-align.
- [Roboto Mono](https://fonts.google.com/specimen/Roboto+Mono) and
  [Arimo](https://fonts.google.com/specimen/Arimo) (Apache-2.0), embedded in the UI;
  Arimo stands in for Arial where it isn't installed.
- [lucide](https://lucide.dev) icons (ISC), embedded as SVG paths in
  `plugin/ui/core/LucideIcons.h`.

The built-in effects are written from published structures with JUCE and Pedaleira's
own helpers. No Neural DSP code or models are used.

# Matching an effect to a reference unit

How a built-in effect is tuned to sound like the same effect on a reference unit (the Quad
Cortex), using nothing but the unit's audio output. A fixed test signal is played through the
unit and through our effect, both outputs are measured, and the numbers are compared. Nothing
of the reference unit is read, copied or reverse engineered: no code, no models, no data
formats, only what it sends out of its output jack.

The kit is `plugin/include/EffectMatch.h` (the analysis library) and `PedaleiraMatch` (the
command line tool, `tools/match_tool.cpp`). Our side of a comparison is rendered with
`PedaleiraRender`.

```
cmake --build build --target PedaleiraMatch PedaleiraRender
build/tools/PedaleiraMatch_artefacts/Release/PedaleiraMatch ...
```

## The test signal

Version 1, mono, 48 kHz, 32-bit float, about 60 seconds. `PedaleiraMatch signals` writes it and
a manifest of its segments. The version is part of the file name; a new version is a new file,
and analyses made with different versions cannot be compared.

| Segment | What it is | What it measures |
|---|---|---|
| `syncStart`, `syncEnd` | A chirp up, a short gap, a chirp down, -6 dBFS, 120 ms. At 0.25 s and at the very end | Latency (the start marker) and clock drift between the two interfaces (the spacing of the two) |
| `sweepLow` | Exponential sine sweep, 20 Hz to 20 kHz, 8 s, -18 dBFS peak, then 4 s of silence | Frequency response, level, echoes (delay time, feedback), reverb decay |
| `sweepHigh` | The same at -6 dBFS | The same at a hotter level, so a level dependent response (drive, compression) shows |
| `sine100`, `sine1k` | 6 s sines, -18 dBFS, then 1 s of silence | Harmonic distortion, tremolo and vibrato (rate and depth) |
| `staircase` | 1 kHz from -60 to 0 dBFS in 3 dB steps, 300 ms each (21 steps) | The static curve: output level against input level (threshold, ratio, knee, saturation) |
| `steps` | 1 kHz at -40 dBFS for 1 s, -10 dBFS for 1 s, -40 dBFS for 2 s | Attack and release |
| `burst` | 200 ms of pink noise at -12 dBFS RMS, then 4 s of silence | How long a tail rings and how it decays (reverb, delay) |
| `silence` | 3 s of nothing | Noise floor |

Every segment leaves a gap of silence after it for tails; the sweeps and the burst leave 4 s.
The full layout, with every position in samples, is in the manifest
(`pedaleira-test-signal-v1.json`).

```
PedaleiraMatch signals --out match/signal
```

## Recording session

### Routing (Focusrite Volt 876)

```
interface line out  ->  reference unit input
reference unit output  ->  interface line in
```

- Play `pedaleira-test-signal-v1.wav` from the interface's output (any player that sends it
  untouched: no volume normalisation, no sample rate conversion, no DSP, system volume at
  the level that was calibrated below).
- Record the reference unit's output on one input at 48 kHz, 24-bit or 32-bit float, mono (a
  stereo file is analysed too, left and right separately, and reported with a width figure).
  Record at the interface's sample rate and do not convert; a recording that is not at the
  rate the signal was made at is refused.
- No other processing in the chain: the reference unit should hold only the effect under
  test, with its amp, cab, gate and every other block off or bypassed, its input and output
  levels set to unity or noted down (see below).
- Start recording before playing the signal and stop 2 seconds after it ends. The recording
  has to contain both sync markers; everything else is found from them.

Levels matter. The sweeps, the sines and the staircase are meant to be measured as the unit
sees them, so set the interface so that the signal reaches the unit at its nominal level
(an instrument or line level setting that the unit treats as 0 dB), and note the unit's input
and output level settings in the file name or a notes file. A recording that is simply
quieter or louder shows up as a flat level offset (`level gain at 1 kHz`); the response curve
is normalised to 1 kHz and does not care. Do not let the interface input clip at the
staircase's 0 dBFS steps.

### What to record

One recording per effect setting. A setting is one position of the effect's controls; the
parameters not under study stay put between recordings. Name the files `<effect>-<setting>.wav`:

```
delay-analog-250ms-fb35.wav
delay-analog-500ms-fb35.wav
delay-analog-750ms-fb35.wav       a delay at three time settings
comp-ratio4-thr-30.wav
comp-ratio10-thr-30.wav           a compressor at two ratios
reverb-hall-decay3.wav
```

Suggested sets: a delay at three times and two feedback settings; a compressor at two or
three ratios and two attack/release settings; a reverb at two or three decay times; a
modulation effect at two rates and two depths; a drive at three gain settings. Also record
the effect bypassed once (`bypass.wav`): that is the unit's own path (interface converters and
the unit's input and output stages), and the first thing to compare against `identity` when
something looks off.

Dry and wet: with the unit's mix at 100% wet the dry sound is absent, which makes the delay
and decay figures cleaner; with a mix in the middle the figures are relative to the dry
sound. Say which in the file name (`-wet`, `-mix50`) and render ours the same way.

### Where to keep them

A `match/` folder outside the repository (recordings are big and are the user's, not ours):

```
match/signal/pedaleira-test-signal-v1.wav
match/ref/delay-analog-250ms-fb35.wav
match/ours/delay-analog-250ms-fb35.wav
match/analysis/ref-delay-analog-250ms-fb35.json
```

Only small, trimmed references that a test needs may go under `test/files/match/`, and they
are never regenerated: a reference recording is a measurement of a unit as it was that day.

## Rendering ours

The same file goes through our effect with `PedaleiraRender`, which reads and writes WAV
files and compensates the rig's latency, so the render lines up with the input:

```
PedaleiraRender --preset delay-analog.json --in match/signal/pedaleira-test-signal-v1.wav \
                --out match/ours/delay-analog-250ms-fb35.wav --tail 4
```

`--tail 4` renders 4 seconds past the end of the signal so that tails and the closing marker
are in the file. The rig can come from a preset (`--preset`: a `.t3kpreset`, or a JSON preset
saved from the app or written by hand, see `docs/library.md`), from a command script
(`--run commands.json`, see `docs/commands.md`), or a preset edited by a script.
A minimal JSON preset holding one effect at known settings:

```json
{
  "format": "pedaleira.preset", "version": 1, "schemaVersion": 1,
  "name": "delay-250", "id": "delay250",
  "chain": {
    "stereoEnabled": false, "branchSide": "left", "branchAfterBlockId": "",
    "children": [
      { "node": "ChainBlocks", "children": [
        { "node": "ChainBlock", "id": "b1", "type": "kind", "enabled": true, "mix": 0.5,
          "children": [
            { "node": "KindState", "kind": "delay.simple", "version": 1, "name": "Simple Delay",
              "children": [
                { "node": "Param", "id": "timeMs", "value": 250.0 },
                { "node": "Param", "id": "feedback", "value": 35.0 }
              ] } ] } ] },
      { "node": "RightChainBlocks" }
    ]
  },
  "params": {}
}
```

The parameters of each effect (ids, ranges, units) come from the `kinds.list` command
(`docs/commands.md`) or from the effect's source under `plugin/src/effects`. The block's `mix` follows the kind's mix law: for a
dry-preserving effect (delays, reverbs) 0.5 keeps the dry sound at unity and brings the echoes in
at unity, 1.0 is wet only.

## Analysing and comparing

```
PedaleiraMatch analyse --in match/ref/delay-analog-250ms-fb35.wav --out match/analysis/ref-delay-analog-250ms-fb35.json
PedaleiraMatch analyse --in match/ours/delay-analog-250ms-fb35.wav --out match/analysis/ours-delay-analog-250ms-fb35.json
PedaleiraMatch compare --reference match/analysis/ref-delay-analog-250ms-fb35.json \
                       --candidate match/analysis/ours-delay-analog-250ms-fb35.json \
                       --markdown match/analysis/delay-analog-250ms-fb35.md
```

`compare` takes the two WAV files directly as well (it analyses them first). It prints the
comparison as JSON on stdout (or to `--out`), writes the table to `--markdown`, and says the
score on stderr. `--strict` makes it exit with 2 when a judged metric is out of tolerance,
for scripts. `analyse` fails with a message saying what to look at when the sync markers are
not found (wrong routing, too quiet, a recording that is cut short, a different version of
the signal, a different sample rate).

The analysis is a plain JSON file that can be kept next to the recording; comparing analyses
is cheap, so keep them.

## What the numbers mean

All figures are measured from the audio, so they include everything the unit does to it. A
figure that is not there (`null`) means the signal showed nothing to measure: no echoes, no
tail, no modulation.

**Alignment.** `latencyMs` is how late the first sync marker arrived. It is the delay of the
whole path (interface converters, the unit's own latency, the effect's lookahead) and is
reported but not judged by default: our render has its latency compensated, theirs has not.
`driftPpm` is the difference between the two interfaces' clocks over the length of the
signal (positive: the recording ran longer than the signal). Segment positions are corrected
by it; anything beyond about 100 ppm deserves a look at the clocking.

**Sweeps** (`sweepLow`, `sweepHigh`). The impulse response comes from the sweep by Farina's
deconvolution. All the figures below are read from it.
- `responseDb`: the early response (the direct sound and the 100 ms after it) in 28 third
  octave bands from 31.5 Hz to 16 kHz, in dB, normalised to the 1 kHz band. Echoes and the
  reverb tail beyond 100 ms are not in it, so a delay does not put a comb in the curve unless
  its time is shorter than 100 ms.
- `levelGainDb`: the level of the 1 kHz band against the same band of the unprocessed sweep.
  0 dB is unity. `sweepHigh` against `sweepLow` shows level dependence.
- `echoes`: peaks of the impulse response after the direct sound (the first peak), up to
  eight, with their time after the direct sound and their level relative to it (energy in
  a 4 ms window). With the effect wet only, the first echo is the direct sound.
- `delayMs`, `feedbackDb`: when the gaps between the direct sound and the echoes agree within 15%, the
  delay time is their median gap, and the feedback is the median level step from one echo
  to the next (-6 dB is 50% feedback). Otherwise they are `null` (a reverb, or a delay with
  its time modulated beyond 15%).
- `decay`: EDT and T30 by Schroeder backward integration of the impulse response from 10 ms
  after the direct sound, both extrapolated to 60 dB (`edtS` from the first 10 dB, `t30S` from -5 to
  -35 dB). They need the tail to fall well above the noise, so a long reverb (more than about 3
  s) or a noisy recording gives `null`. Left out for a clean echo train, where the decay
  curve is a staircase.

**Sines** (`sine100`, `sine1k`), read from the 5 s in the middle of each.
- `thdPct`: harmonics 2 to 10 over the fundamental, as a percentage. Noise is not included.
  The floor of a clean path is a few thousandths of a percent.
- `ampDepthDb`, `ampRateHz`: peak to peak variation of the level in dB (the 2nd to 98th
  percentile of the envelope), and the dominant rate of that variation (tremolo, an
  auto-wah). The rate is `null` below 0.05 dB.
- `pitchDepthCents`, `pitchRateHz`: the same for the instantaneous frequency in cents
  (vibrato, chorus, rotary). The rate is `null` below 0.5 cents.

**Static curve.** The level of the 1 kHz component in the last 100 ms of each of the 21
staircase steps, in dBFS, against the input level. A compressor shows as a knee and a slope
below 1 above the threshold; a limiter as a flat top; a drive as rounding near the top.

**Steps.** `attackMs` and `releaseMs`: the time for the output level to cover 63% of its
way from where it is right after the step (1 ms in) to where it settles. Up from -40 to -10
dBFS is the attack, back down is the release. For a compressor that is its time constant
(the reduction is a one-pole), not the 10 to 90 time. `null` when the level moves less than
1 dB, or never reaches the 63% mark.

**Burst.** `tailEndMs`: from the end of the burst until the output stays more than 60 dB below
its level during the burst (`truncated` if the recording ended before that). `tailT60s`: the decay time of the tail,
extrapolated to 60 dB, from the Schroeder integral of the tail.

**Noise floor.** `noiseFloorDb`: the level of the silence segment, dBFS RMS (the lower of its
overall level and that of its quietest tenth of 50 ms windows).

**Stereo** (stereo recordings only). `stereo.correlation` and `stereo.width` over the burst
and its tail: correlation 1 and width 0 is mono, near 0 correlation and 0.7 width two
unrelated channels.

### Comparison tolerances

A metric passes when the candidate is within the allowed difference of the reference. The
defaults (`EffectMatch::Tolerances`; the library accepts others) are:

| Metric | Allowed |
|---|---|
| Response, per band | 1 dB |
| Level gain, sine levels | 1 dB |
| Delay time | 1 ms |
| Feedback | 1 dB |
| EDT, T30 | 10% (at least 0.02 s) |
| THD | 25% (at least 0.05 percentage points) |
| Modulation rate | 5% (at least 0.05 Hz) |
| Modulation depth | 10% (at least 0.1 dB, 0.5 cents) |
| Static curve, per step | 1 dB |
| Attack, release | 20% (at least 2 ms) |
| Burst tail T60, tail end | 10% (at least 0.05 s, 20 ms) |
| Noise floor | 3 dB |
| Stereo correlation | 0.1 |
| Latency | not judged (1 ms when asked) |
| Clock drift | not judged |

A metric that exists in one analysis and not in the other fails (one has echoes and the other
does not). Metrics that neither has are skipped. The score is the share of judged metrics that
passed, 0 to 100. The markdown starts with the failures, then lists every metric by group.

## Reading a result

- A flat level difference with a good response shape: a gain stage (output level, makeup).
- A response that differs by a smooth tilt or a bump: a filter (tone, damping, the unit's
  input and output stages; compare against `bypass.wav` first).
- Delay time right and feedback off by a constant: the feedback law differs; off by a
  growing amount per echo: filtering in the loop.
- Static curve right at high levels and off at low ones, or the other way: threshold, knee or
  makeup. Slope above the threshold is 1/ratio.
- Rates right, depths off: the modulation depth law differs from ours.

## Limits

- Black box, so a number says what an effect does, not why. Match the numbers, then listen.
- The impulse response of a nonlinear effect is its response at that level; the sweeps at
  two levels show how it changes, nothing more.
- Time varying effects (chorus, flanger, phaser, rotary) have no single impulse response; their
  sweep figures are an average and the sine figures carry the modulation.
- Resolution: attack and release 1 ms, modulation rate about 0.1 Hz, delay time about 0.1 ms.
- A clock that runs off is corrected by scaling the positions of the segments, not by
  resampling the recording. That keeps every segment where it should be, but the sweep is then
  slightly longer or shorter than the filter that decodes it (at 100 ppm the top of the sweep
  arrives 0.8 ms late against the bottom, a change of phase and not of level). Interfaces are
  well within 50 ppm; if `driftPpm` is far beyond that, fix the clocking and record again.

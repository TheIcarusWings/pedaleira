# Writing a built-in effect

The brief for adding a block kind under `plugin/src/effects`. The reference is
`plugin/src/effects/delay/SimpleDelay.cpp` with `test/src/effects/simple_delay_tests.cpp`:
copy their shape. Everything named here is documented in its header.

## Files

| What | Where |
|---|---|
| The effect, one file | `plugin/src/effects/<family>/<Name>.cpp` |
| Its tests, one file | `test/src/effects/<name>_tests.cpp` |
| Helpers private to a family (no kinds) | `plugin/src/effects/<family>/detail/*.cpp`, headers in `plugin/include/effects/<family>/` |
| Shared DSP (do not add without asking) | `plugin/include/effects/dsp/`, `plugin/src/effects/dsp/` |

Families: `dynamics`, `eq`, `filter`, `delay`, `reverb`, `modulation`, `pitch`, `drive`.
Both folders are globbed (`CONFIGURE_DEPENDS`): adding a file needs no CMake edit and no
edit to any shared source (the list of hooks is generated into the build folder). Never
touch `BlockKinds.cpp`, `Processor.cpp`, the harness or a CMake file for an effect; if
the framework is missing something, ask. The file name must be a C++ identifier and unique across all families
(`SimpleDelay.cpp`, not `Delay.cpp` in two folders). The class lives in an anonymous
namespace in the `.cpp`; no public header.

## Kind id and registration

- Kind id: `<category>.<name>`, lower case, digits and dots only: `delay.simple`,
  `dynamics.compressor`. The category part must be the kind's own `BlockCategory`. Ids are
  saved in presets and never change.
- Every effect `.cpp` (outside `dsp/` and `detail/`) defines one hook named after the file:

```cpp
PEDALEIRA_EFFECT_KINDS(SimpleDelay) {   // file is SimpleDelay.cpp
  addEffectKind<SimpleDelayProcessor>(registry, "Simple Delay", BlockCategory::Delay, 0.35f,
                                      BlockMixLaw::DryPreserving);
}
```

  A file may register several kinds. Use the `registry` argument, never
  `BlockKindRegistry::instance()`. The last two arguments are the mix a new block starts
  at and the mix law (see Mix); both default to a kind that replaces its input (1,
  `Crossfade`).
- Forgetting the hook is a link error (`undefined registerKindsIn_<FileStem>`). A hook
  that registers nothing, an id the registry refuses, and a kind without a conformance
  test fail in `EffectRegistrationTest`.

## The class

Derive from `ParameterisedBlockProcessor` and include `effects/EffectDsp.h`. Provide
`static constexpr const char* kKindId`, `static const BlockParamTable& parameterSpecs()`
(a function-local static), an `enum Param` in table order, a default constructor, then:

| Override | Thread | Rule |
|---|---|---|
| `prepareEffect(spec)` | not audio | Allocate and size everything here; end with `resetEffect()`. |
| `resetEffect()` | audio | Clear delay lines, filters, envelopes, LFO phase; `snapOnNextTarget()` on smoothers; reseed noise. No allocation. |
| `processEffect(buffer, ctx)` | audio | In place. Read values with `param(kIndex)`. |
| `tailSeconds()` | any, audio for spillover kinds | Atomics and arithmetic only. |
| `latencySamples()` etc. | see `BlockProcessor.h` | Only for look-ahead designs. |

What `processEffect` gets:
- **Always 48 kHz**, whatever the host rate or the oversampling (effects run at the base
  rate). Size from `spec.sampleRate` anyway.
- Blocks of any length from 1 to `spec.maxBlockSize`, varying call to call.
- **Wet only.** Output the processed signal; the block's common `mix`, `inputGain`,
  `outputGain`, EQ and bypass are applied by the chain. Do not add a mix parameter.
- **One or two channels** (see Channels). Loop over `buffer.getNumChannels()` and keep
  per-channel state.

## Channels

| Buffer | `ctx.dualMono` | Meaning | When |
|---|---|---|---|
| 1 channel | false | Mono. | The host bus is mono, or the rig is in its two-lane stereo mode (each lane is one channel). |
| 2 channels | false | **True stereo.** | Two-channel bus, one chain, input mode not dual: the default. |
| 2 channels | true | Two independent signals. | Two-channel bus, one chain, input mode dual (two guitars through one rig). |

- In true stereo the processor folds its input to mono before the chain, so the first
  block receives the same audio on both channels; a block behind a stereo IR or a stereo
  effect receives channels that differ. What a block writes to each channel is what is
  heard left and right.
- Implement stereo behaviour (ping-pong, decorrelated reverb, LFO phase offset between
  the sides, width) **only when `blockHasStereoImage(buffer.getNumChannels(), ctx)`**: two
  channels, not dual mono, and no NAM block later in the lane. An amp model reads channel
  0 and overwrites channel 1, so stereo made before it is lost; there, run the mono
  version on both channels.
- With `ctx.dualMono` set, never mix, sum, pan or cross-feed, and treat both channels
  exactly alike: each must come out as a mono rig would render it (the conformance test
  compares them).
- With one channel, degrade to the mono version of the effect: same level, same timing.

## Mix

`BlockMixLaw` says what the common mix knob (0 to 1) does for the kind:

| Law | Dry gain | Wet gain | For |
|---|---|---|---|
| `Crossfade` (default) | 1 - mix | mix | Kinds whose wet replaces the dry: dynamics, EQ, filter, drive, pitch, modulation. |
| `DryPreserving` | min(1, 2(1 - mix)) | min(1, 2 mix) | Kinds heard beside the dry: **delays and reverbs.** |

With `DryPreserving` the dry signal stays at unity up to 50%, where the wet reaches unity
too, and only fades above that (wet alone at 100%); pair it with a default mix of 0.25 to
0.4. Use `Crossfade` for everything else unless you can argue the other (a chorus that
outputs only its voices would be a case; one that includes the dry signal is not).

## Parameters

`BlockParamSpec::floatParam(id, name, unit, min, max, default)`, `boolParam`, `choiceParam`.

- Ids are lowerCamel, permanent, unique in the kind, and not a common id (`enabled`,
  `normalize`, `inputGain`, `outputGain`, `mix`). Put the unit in the id when it helps:
  `timeMs`, `toneHz`, `thresholdDb`.
- Ranges and defaults in real units; every float has a unit (`dB`, `ms`, `Hz`, `%`, `st`,
  `ratio`). Amounts are 0 to 100 `%`, not 0 to 1. The default must lie on the grid.
- `.withSkew(0.3..0.5)` for frequencies and times; `.withStep(1)` for integers.
- Flags: `.withAffectsTail()` on anything that changes `tailSeconds()`;
  `.withAffectsLatency()` on anything that changes latency (then follow the latching
  contract in `BlockProcessor.h`); `.notAutomatable()` on switches that cannot change
  click-free (algorithm or mode choices); `.withTempoSync()` is set for you by the helpers
  below.
- Never reorder, rename or remove a parameter once merged; append, and bump
  `kindStateVersion()` if meaning changes.

## Readings

A value the effect reports while it runs, for a meter (the dynamics kinds report
`gainReductionDb`). Declare a `static const BlockReadingTable& readingSpecs()` beside
`parameterSpecs()`, each entry `{id, name, unit, min, max}`, and pass it to the base:
`ParameterisedBlockProcessor(kKindId, parameterSpecs(), readingSpecs())`. Registration
picks it up; `kinds.list` lists it and `meters.get` returns each block's values under
`readings`.

- Call `setReading(index, value)` once at the end of `processEffect`, with the block's
  most telling value (the most reduction in the block, not the last sample's).
- It is clamped to the range and goes back to `min` on reset (the block stopped being
  processed), so `min` is the resting value.
- Ids follow the parameter rules and are as permanent.

## Helpers (`namespace fx`, `plugin/include/effects/dsp/`)

| Header | Use |
|---|---|
| `Smoothing.h` | `SmoothedParam`: `prepare(rate, seconds)`, `setTarget(param(i))` once per block, `next()` per sample. Snaps on the first target after prepare/reset. |
| `DelayLine.h` | `prepare(channels, maxSamples)`, `write`, `read`, `readLinear`, `readHermite` (use Hermite when the delay moves). |
| `Lfo.h` | Sine, triangle, sample-and-hold; `valueAt(offsetTurns)` for the right channel in true stereo; `setRateFromTempo`, `syncPhaseToTransport`. |
| `Filters.h` | `OnePole`, `StateVariableFilter` (anything swept), `Biquad` + `BiquadCoeffs` (fixed EQ shapes). One instance per channel. |
| `EnvelopeFollower.h` | Peak or RMS with attack and release. |
| `TempoSync.h` | Note values; `appendTimeParams` / `appendRateParams` and `resolveTimeSeconds` / `resolveRateHz`. |
| `Saturation.h`, `Levels.h` | `fastTanh`, `softClip`, `dbToGain`, `gainToDb`, `flushDenormal`. |
| `Noise.h` | `NoiseSource(salt)`: fixed-seed noise. Never `juce::Random` or the clock. |

Do not use `juce::dsp::IIR` or anything else that allocates when its settings change.

## Realtime rules

In `processEffect`, `resetEffect` and (spillover kinds) `tailSeconds`: no allocation, no
locks, no logging, no file or system calls, no exceptions as control flow. Buffers are
sized in `prepareEffect`. Denormals are already flushed on the audio threads. Smooth
every parameter whose jump would click. Update control-rate values (filter coefficients,
LFO-driven delays) per sample or on a fixed internal grid of 16 or 32 samples, never
"once per host block": the output must not depend on the block size, and the conformance
test checks it.

## Time, tails, latency, spillover

- **Tempo.** A time or a rate uses the standard three parameters, so every effect reads
  the same: `sync` (bool), `division` (choice of 18 note values, 1/1 to 1/32, straight,
  dotted, triplet) and `timeMs` or `rateHz`. Add them with `fx::appendTimeParams` /
  `fx::appendRateParams` (three consecutive entries, in that order) and read them with
  `fx::resolveTimeSeconds(param(kSync), param(kDivision), param(kTimeMs), ctx.transport)`.
  A second time in one effect takes an id prefix (`leftSync`, ...). `ctx.transport.bpm`
  is the host's tempo or the rig's own; `playing` and `ppqPosition` are only meaningful
  under a host.
- **Tail.** `tailSeconds()` is how long the wet output stays above -60 dB after the input
  stops, from the current parameters; a close upper bound, never too short. It is also how
  long hosts keep rendering and how long a released block keeps costing CPU, so do not pad
  it. It has no context: a synced kind keeps the last tempo it saw in an atomic. Return
  the honest value even when it is minutes: the chain reads it through
  `reportedTailSeconds()`, capped at `kMaxBlockTailSeconds` (30 s, as for hosted plugins),
  and fades out whatever still rings there. Do not cap it yourself.
- **Latency.** Report 0 unless the wet path truly lags its input (look-ahead). An echo or a
  chorus voice is not latency.
- **Spillover.** A kind whose tail should ring out after a bypass puts `spilloverParam()`
  last in its table. That is the whole declaration. The chain then closes the block's
  input, keeps running it and adds its wet output (at the level the mix gave it) to the
  dry signal at unity until the reported tail has passed, calls `resetEffect()` and stops
  processing it. Re-enabling fades the input
  back in. With the block's `spillover` off, and for every other kind, a bypass is a 25 ms
  fade followed by `resetEffect()`. A scene that turns the block off takes the same path
  (see `docs/scenes.md`). Removing a block cuts its tail.

## Tests

`test/src/effects/<name>_tests.cpp` includes `effects/effect_test_helpers.h` (namespace
`fxtest`) and starts with the conformance test:

```cpp
TEST(SimpleDelayTest, Conforms) {
  ConformanceOptions options;
  options.params = {{"timeMs", 120.0}, {"feedback", 40.0}};   // a setting that does something
  options.stateParams = {...};                                // every parameter off its default
  options.latency = LatencyCheck::AtMostOnset;                // wet has no dry in it
  expectKindConforms("delay.simple", options);                // the id as a literal
}
```

Once: registered with the right category and id; parameter table (lowerCamel ids,
defaults in range and on the grid, units, choices); state round trip; CPU (default
ceilings 40,000 ns average and 400,000 ns worst per 128-sample block, printed: tighten
them); dual mono has no cross-feed and each channel equals a mono rig's render (-60 dB);
clean and repeatable on a true stereo buffer whose channels differ.
In each layout (mono, true stereo, dual mono), on both outputs: finite, bounded,
denormal-free under a random parameter sweep; silence after the reported tail and tail
not shorter than the measured decay (50 ms; only a tail at the 30 s cap may be shorter);
reported latency equals the wet onset (1 sample); bypass bit-exact against the empty rig;
bit-identical repeat renders; block 32 vs 512 within -60 dB and 44.1 kHz level within
0.5 dB.

On top, each effect asserts what it is for, using `EffectRig` (one block, built through
the command layer) and the measurements in the harness: `captureImpulseResponse`,
`estimateDelaySamples`, `estimateRt60Seconds`, `magnitudeResponseDb`, `sineProbeGainDb`,
`steadyStateGainDb`, `thdPercent`, `amplitudeModulationRateHz`,
`frequencyModulationRateHz`, `scanSignal`, `maxStep`. For example: a compressor's static
curve and attack/release times, an EQ's response at its corner, a reverb's RT60 against
its decay parameter, a chorus rate against `rateHz` and the synced value at two tempos.
A spillover kind also tests that its tail rings with the switch on and is cut with it off;
a stereo kind tests its width in true stereo and that mono and dual mono fall back.

Harness facts: set `mix` to 1 to measure the wet path alone. The rig is the whole
processor, so a 5 Hz DC blocker follows the chain (a -60 dB, 50 ms tail of its own):
compare against a reference `EffectRig{juce::String()}`, and expect "equal to dry" after
a transition only to about 1e-6, never bit-exact. For a number the rig would blur (a
decay down to -60 dB, an exact gain) run the kind alone with `DirectKind`. Parameters set
on a rig land on the next block; call `rig.restart()` to start from rest at the new
values. `RigOptions::layout` (`rigFor(Layout::Mono | Stereo | DualMono)`) picks the buffer
the block gets; the default is true stereo, and `rig.render(vector)` returns the left
output only, so a stereo effect measures both with `renderBoth`. A one-block rig always
feeds identical channels; `DirectKind(kind, params, Layout::Stereo)` takes a left and a
right that differ. Do not add golden files.

Run: `cmake --build build --target DspTests` then
`./build/test/DspTests_artefacts/Release/DspTests --gtest_filter='SimpleDelay*'`.

## Checklist

- [ ] One `.cpp` under `plugin/src/effects/<family>/`, unique identifier file name, hook defined.
- [ ] Id `<category>.<name>`; name, category, default mix and mix law chosen.
- [ ] Parameters in real units with units, flags set, standard time/rate parameters used.
- [ ] Everything allocated in `prepareEffect`; `resetEffect` clears all audio state.
- [ ] Works on 1 and 2 channels and any block length; stereo behaviour only in true stereo,
      none in dual mono.
- [ ] Every audible parameter smoothed; control updates independent of block size.
- [ ] `tailSeconds()` honest and uncapped; `spilloverParam()` last if the tail should ring out.
- [ ] Test file calls `expectKindConforms("<id>", ...)` and tests the effect's own behaviour.
- [ ] CPU ceilings tightened to about 3x the printed numbers.
- [ ] `ctest --test-dir build -j 8` passes; golden tests untouched; no shared file edited.

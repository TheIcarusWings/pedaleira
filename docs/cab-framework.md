# The cab model

A guitar cab and the mic in front of it, computed from physics instead of loaded from a file.

The `cab.model` block ("Cab") builds the impulse response of a mic'd cab from a handful of
physical descriptions: the speaker (its Thiele-Small parameters, its voice coil, the resonances of
its cone), the box it sits in, the mic, where the mic is, and the room. It plays that response
through the same zero-latency convolution engine the IR blocks use. Turning a control computes a
new response off the audio thread and crossfades to it.

Speakers are data, like amps (`docs/amp-framework.md`): a speaker is a small JSON file, six of them
ship in `resources/cabs`, and you can describe your own.

```
resources/cabs/*.json                       the shipped speakers, as data (linked into the binary)
plugin/include/cab/SpeakerDescription.h     a speaker as a structure (plain C++)
plugin/include/cab/CabResponse.h            the model: response and impulse response (plain C++)
plugin/src/effects/cab/dsp/                 the model, the loader and the builder thread
plugin/src/effects/cab/CabModel.cpp         the cab.model kind
plugin/include/effects/cab/BuiltinCabs.h    the shipped speakers, parsed once
test/src/effects/cab_model_tests.cpp        its tests
```

Contents: [the block](#the-block), [the model](#the-model), [the speaker description format](#the-speaker-description-format),
[the shipped speakers](#the-shipped-speakers), [describing your own speaker](#describing-your-own-speaker),
[the cabinets](#the-cabinets), [the mics](#the-mics), [exporting a cab as an IR](#exporting-a-cab-as-an-ir),
[an amp's default cab](#an-amps-default-cab), [real time](#real-time), [what is not modelled](#what-is-not-modelled),
[licences](#licences).

## The block

| Id | Name | Range | Default | What it moves |
|---|---|---|---|---|
| `speaker` | Speaker | Granite, Ember, Velvet, Brass, Oak, Flint, Custom | Granite | The speaker description. Custom is the one loaded with `block.loadDescription`. |
| `cabSize` | Cab | 1x12, 2x12, 4x12, 1x10, 2x10 | 4x12 | The box: its volume, its baffle, how many speakers and where. |
| `back` | Back | Closed, Open | Closed | A sealed box, or an open back with a wall behind it. |
| `mic` | Mic | Dynamic, Ribbon, Condenser | Dynamic | The mic type. |
| `micPosition` | Position | 0 to 100 % | 35 % | Where the mic points: 0 at the dust cap, 100 at the edge of the cone. |
| `micDistanceCm` | Distance | 0.5 to 100 cm | 2.5 cm | From the grille cloth to the capsule. |
| `micAngleDeg` | Angle | 0 to 60 deg | 0 | The mic's tilt off the speaker's axis. |
| `room` | Room | 0 to 100 % | 10 % | The early reflections of a small room. |
| `lowCutHz` | Low cut | 20 to 400 Hz | 30 Hz | A 12 dB per octave high-pass (Butterworth). |
| `highCutHz` | High cut | 2 to 20 kHz | 16 kHz | A 12 dB per octave low-pass (Butterworth). |
| `levelDb` | Level | -24 to +12 dB | 0 dB | The block's output level (not part of the response). |

Every control but the level changes the response; the level is a plain gain with a 20 ms glide.
The response is normalised so the band from 150 Hz to 3.5 kHz averages 0 dB: moving the mic
changes the tone, not the loudness, as with an IR loader that normalises its files. The block's
category is `cab`, its mix law crossfade (a cab replaces its input), its latency 0, its tail the
length of the response (85 ms).

## The model

`cab::cabFrequencyResponse` evaluates the model at any frequency; `cab::cabImpulseResponse` turns
its magnitude into a minimum-phase impulse response. The response at frequency f is the product

```
H(f) = speaker(f) x paths(f) x mic(f) x cuts(f)
```

where `paths` sums every way the sound gets from the cab to the mic, with its own level, delay
and directivity. Units below: metres, seconds, Hz; k = 2 pi f / c is the wave number, c = 343 m/s.

### The speaker: a lumped electro-mechanical model

Above the speaker's resonance, with a voltage drive, the cone's acceleration is flat; below it,
the suspension (and, in a closed box, the air) holds the cone back. Small-signal theory reduces
this to a second-order high-pass, from the Thiele-Small parameters (A. N. Thiele, "Loudspeakers
in vented boxes", JAES 19, 1971; R. H. Small, "Direct-radiator loudspeaker system analysis",
JAES 20(5), 1972):

```
speaker(s) = s^2 / (s^2 + s wc / Qc + wc^2)
```

- **Closed back**: the air in the box is a second spring in parallel with the suspension. With
  alpha = Vas / Vb (Vas the speaker's compliance volume, Vb the box's volume per speaker) the
  resonance and the Q both rise by sqrt(1 + alpha): fc = fs sqrt(1 + alpha), Qc = Qts sqrt(1 + alpha)
  (R. H. Small, "Closed-box loudspeaker systems, part I: analysis", JAES 20(10), 1972). The low end
  is tighter and carries a defined bump at fc; below it the response falls at 12 dB per octave.
- **Open back**: nothing behind the cone, so the free-air resonance fs and Qts hold, and the cone's
  rear wave is free to come round the box (see the paths below).

The voice coil: the force on the cone follows the current, which falls as the coil's impedance
rises. A coil in an iron gap is lossy; its impedance grows as f^n with n below 1 rather than as f
(W. M. Leach, "Loudspeaker voice-coil inductance losses", JAES 50(6), 2002):

```
coil(f) = Re / (Re + K (j w)^n),   K set so |K (j w)^n| = 2 pi 1000 Le at 1 kHz
```

The cone in breakup: above about 1 kHz a paper cone stops moving as one; its modes give the peaks
and dips that are most of a guitar speaker's voice. Each is a second-order resonator,

```
(s^2 + s (A / Q) w0 + w0^2) / (s^2 + s / (A Q) w0 + w0^2),   A = 10^(gain / 40)
```

and above them the cone stops radiating: a Butterworth roll-off of the description's order.

A speaker described at one diameter and played in a cab for the other is scaled: its modes and its
resonance move by the ratio of the diameters (a smaller, lighter cone is stiffer for its mass) and
its Vas by the fourth power of it (Vas grows as the square of the cone area).

### The paths: speakers, baffle, rear wave, floor

Every speaker of the cab is a source on the baffle, its acoustic centre half way down the cone
(the cone's depth is 3.75 mm per inch of diameter). The mic is in front of speaker 0; the position
moves it across that cone towards the middle of the box. For each speaker j:

- **Direct sound**, with spreading, delay and directivity:
  `D_j(f) / R_eq(r_j) x exp(-j k r_j)`. Up close, a speaker is not a point: the pressure in front of
  a piston of radius a at low frequencies is that of a point source at
  `R_eq(r) = a^2 / (2 (sqrt(r^2 + a^2) - r))`, a / 2 at the cone and r far away (the on-axis near
  field of a baffled piston; D. B. Keele, "Low-frequency loudspeaker assessment by nearfield
  sound-pressure measurement", JAES 22(3), 1974). This is why a close mic hears its own speaker far
  above the others, and why the others come in as the mic backs off.
- **Directivity**: the piston's `2 J1(x) / x` with `x = k a(f) sin(theta)`, as its smooth envelope
  (-3 dB at x = 1.62, then x^-1.5, since a real cone has no clean nulls), times the obliquity
  `(1 + cos theta) / 2` of a source sitting behind the baffle in its frame. The radiating radius
  a(f) is the whole piston low down and the description's `radiating_fraction` of it once the cone
  breaks up (`a(f) = a (q + (1 - q) / (1 + (f / f_b)^2))`, f_b 1 kHz on a 12 inch cone). The angle
  is taken from the near side of the inner half of that disc, so a mic over the moving part of the
  cone is in its beam: **centre is bright, edge is dark**, and the position matters up close and
  hardly at all at a metre.
- **Baffle edges**: each of the four edges re-radiates half the wave that reaches it with the
  opposite sign (J. Vanderkooy, "A simple theory of cabinet edge diffraction", JAES 39(12), 1991),
  shared between the edges, with the path speaker to edge to mic. Far away this is the 6 dB baffle
  step and a ripple; up close it is nothing against the direct sound.
- **Open back**: the rear wave, inverted, travels out of the back and round each edge of the box
  to the mic (`-1/4` per edge, a low-pass at 800 Hz for the partial back panel and the corners),
  and again off a wall 30 cm behind the cab (reflection 0.6). Far away the front and rear waves
  cancel at low frequencies: the **dipole roll-off** of an open back. Up close the direct sound
  dominates and the open back is mostly its lower, looser resonance.
- **Floor**: the cab stands on the floor; the floor is an image source below it (reflection 0.7),
  heard through the mic's polar pattern. With the speaker-to-floor and mic heights of a real cab,
  the reflection is far below the direct sound up close and close to it at a metre: **the comb
  filtering grows with distance**, together with that of the other speakers and the edges.

### The mic

| Mic | Pattern | Low end | Top | Presence | Capsule radius (off axis) |
|---|---|---|---|---|---|
| Dynamic | cardioid, 0.5 + 0.5 cos | 2nd-order high-pass, 180 Hz, Q 0.55 | 2nd-order low-pass, 12.5 kHz | +5 dB at 5.2 kHz, Q 1.1 | 25 mm |
| Ribbon | figure of eight, cos | 2nd-order high-pass, 90 Hz, Q 0.6 | 2nd-order low-pass, 6.5 kHz, Q 0.55 (dark) | none | 26 mm (a tall ribbon) |
| Condenser | cardioid | 2nd-order high-pass, 25 Hz | 2nd-order low-pass, 18 kHz | +1 dB at 11 kHz | 22 mm |

- **Proximity effect**: the pressure-gradient part of a first-order mic sees the wave front's
  curvature, `1 + 1 / (j k r)`, so a dynamic or a ribbon close to the cone gets a bass boost (a
  ribbon, all gradient, the most). r is the speaker's equivalent distance R_eq, so the boost stops
  growing once the mic is on the cone (J. Eargle, *The Microphone Book*, 2nd ed., 2004, chapter 4).
  The dynamic's own low end is tuned down to offset it at the usual close distance, as such mics are.
- **Angle**: the pattern at the tilt, with the gradient part's proximity effect scaled by cos, and
  the capsule's high-frequency loss off axis: the same piston envelope with the capsule's radius.
  **Off axis loses highs.**
- Reflections (floor, room) arrive from other directions and are weighed by the pattern at their
  angle of arrival (a cardioid hardly hears the wall behind it; a ribbon hears it in full, inverted).

### The room

`room` scales the reflections of a small room (2.1 m to the side walls either side of the cab,
a 2.6 m ceiling, a wall 4.2 m in front) from 0 to a reflection coefficient of 0.9: the first-order
images in the side walls, the ceiling and the front wall, and each of those again in the floor
(J. B. Allen and D. A. Berkley, "Image method for efficiently simulating small-room acoustics",
JASA 65(4), 1979). Each bounce is a one-pole low-pass at 5 kHz (wall and air absorption). The
reflections arrive 5 to 30 ms after the direct sound: early reflections only, no reverb tail.
A close mic hears a real room very little, so the room is heard as if the mic were at least a metre
away (its level relative to the direct sound is scaled up by `max(R_eq, 1) / R_eq`); its timing
follows the real position.

### From the response to an impulse response

The magnitude is evaluated on the bins of a transform four times the length of the response,
normalised (150 Hz to 3.5 kHz at 0 dB, power, on a log frequency axis), floored at -120 dB, and
made minimum phase by the real cepstrum (fold the cepstrum onto positive quefrencies and
exponentiate; A. V. Oppenheim and R. W. Schafer, *Discrete-Time Signal Processing*, 3rd ed.,
section 12.8). A minimum-phase response puts all its energy as early as the magnitude allows: no
pre-ringing, no latency, and the delays of the reflections are kept, since a sum of a direct
sound and weaker delayed copies is already minimum phase. The response is
`cab::cabImpulseLength(rate)` samples (4096 at 48 kHz, 85 ms), faded out over its last quarter.

## The speaker description format

A speaker description is one JSON file, format `pedaleira-cab-speaker`, version 1. The loader
(`plugin/src/effects/cab/dsp/SpeakerDescription.cpp`) checks every field's type and range, rejects
unknown fields, and says where a fault is: `cone.breakup[2].q: must be between 0.2 and 30, not 40`.
Granite, with two of its five resonances left out:

```json
{
  "format": "pedaleira-cab-speaker",
  "version": 1,
  "metadata": {
    "name": "Granite",
    "author": "Pedaleira",
    "licence": "MIT",
    "summary": "Tight and forward: ..."
  },
  "diameter_in": 12,
  "thiele_small": { "resonance_hz": 82, "q_total": 0.9, "compliance_l": 40 },
  "voice_coil": { "resistance_ohm": 6.4, "inductance_mh": 0.4, "loss_exponent": 0.65 },
  "cone": {
    "radiating_fraction": 0.5,
    "breakup": [
      { "frequency_hz": 700, "gain_db": -3.5, "q": 1.0 },
      { "frequency_hz": 2500, "gain_db": 6.5, "q": 2.5 },
      { "frequency_hz": 4000, "gain_db": 6.5, "q": 3.0 }
    ],
    "rolloff_hz": 5600,
    "rolloff_order": 5
  }
}
```

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `format` | string | | | | required | `"pedaleira-cab-speaker"`. |
| `version` | integer | | | | required | 1. |
| `metadata.name` | string | | | | required | The speaker's name; shown in the speaker choice when loaded as Custom, and what an amp's `default_cab.speaker` names. |
| `metadata.author` | string | | | | | Who wrote the description. |
| `metadata.licence` | string | | | | required | The licence of this file (an SPDX identifier or a name). |
| `metadata.summary` | string | | | | | What the speaker sounds like. |
| `metadata.references` | array of strings | | | | | Where the values come from. |
| `diameter_in` | number | inch | 6 to 15 | | required | The nominal diameter the values describe. In a cab of the other size (10 or 12) the cone is scaled. |
| `thiele_small.resonance_hz` | number | Hz | 20 to 300 | | required | fs, the free-air resonance. |
| `thiele_small.q_total` | number | | 0.2 to 5 | | required | Qts, the total Q at fs. Guitar speakers sit between about 0.5 and 1.5: higher is a looser, boomier bottom and a bigger bump in a closed box. |
| `thiele_small.compliance_l` | number | litre | 1 to 500 | | required | Vas, the volume of air as compliant as the suspension. Against the box volume it sets how far a closed box raises the resonance. |
| `voice_coil.resistance_ohm` | number | ohm | 1 to 32 | 6.4 | | Re, the coil's DC resistance. |
| `voice_coil.inductance_mh` | number | mH | 0 to 10 | 0 | | The coil's inductance, as the inductance with its reactance at 1 kHz. 0 leaves it out. |
| `voice_coil.loss_exponent` | number | | 0.3 to 1 | 0.65 | | n in K (j w)^n: 1 is an ideal inductor, real coils in iron are 0.5 to 0.8. |
| `cone.radiating_fraction` | number | | 0.1 to 1 | 0.5 | | The part of the cone's radius still moving as one at the top of the range. Smaller beams more: darker off axis and towards the edge. |
| `cone.breakup` | array | | at most 16 | | | The cone's resonances: `frequency_hz` (50 to 20000), `gain_db` (-24 to 24, negative for a dip), `q` (0.2 to 30), optional `note`. |
| `cone.rolloff_hz` | number | Hz | 1000 to 20000 | | required | Where the cone stops radiating. |
| `cone.rolloff_order` | integer | | 1 to 12 | 4 | | The roll-off's order, 6 dB per octave each: 4 is 24 dB per octave. |

## The shipped speakers

Six original voicings, our own data, made to sound good with the amps in `resources/amps`. Each
has its resonance between 70 and 100 Hz, its strongest breakup between 2 and 4 kHz, and little
left above 6 kHz.

| File | Name | Size | fs | Qts | Vas | Character |
|---|---|---|---|---|---|---|
| `granite.json` | Granite | 12 | 82 Hz | 0.9 | 40 l | Tight and forward: a scooped low mid and hard peaks at 2.5 and 4 kHz. Made for high gain. The default. |
| `ember.json` | Ember | 12 | 72 Hz | 1.1 | 48 l | Warm and vocal: a loose, round bottom and a soft, early breakup centred on the upper mids. |
| `velvet.json` | Velvet | 12 | 70 Hz | 0.95 | 55 l | Smooth and dark: one broad breakup peak and an early roll-off. |
| `brass.json` | Brass | 12 | 88 Hz | 0.95 | 36 l | Bright and open: a deep dip in the low mids, glassy peaks near 3 and 4.6 kHz. |
| `oak.json` | Oak | 12 | 78 Hz | 0.9 | 45 l | Balanced and punchy: a low mid hump and even breakup peaks. |
| `flint.json` | Flint | 10 | 100 Hz | 0.85 | 22 l | Sharp and cutting: a lean bottom, thin low mids and a pronounced peak near 4 kHz. |

The order of the speaker choice is fixed (presets store the index); a new shipped speaker is
appended to `kSpeakerFiles` in `plugin/include/effects/cab/BuiltinCabs.h` and its file added to
`resources/cabs` (CMake links every `*.json` there into the binary as `PedaleiraCabData`, see
`cmake/AmpFramework.cmake`).

## Describing your own speaker

You need the speaker's datasheet, or an impedance sweep and an on-axis response of your own.

1. **Size and Thiele-Small parameters.** `diameter_in`, `resonance_hz` (fs), `q_total` (Qts) and
   `compliance_l` (Vas) are printed on most datasheets. Without one, measure the impedance: fs is
   the peak, and the classic added-mass or sealed-box methods give Vas; Qts follows from the peak's
   width (Small 1972).
2. **The voice coil.** `resistance_ohm` is Re (measure it with a meter); `inductance_mh` is Le at
   1 kHz from the datasheet or the impedance rise above resonance.
3. **The cone.** This is the speaker's voice. From a published on-axis response (or your own,
   measured close with a flat mic), list the main peaks and dips between about 500 Hz and the
   roll-off, each with its frequency, height and width (Q is the centre frequency over the width at
   half the height in dB). Put the corner of the final roll-off where the response has fallen about
   3 dB below its trend and pick the order from the slope after it (24 dB per octave is order 4).
   Remember the model adds the coil, the cab and the mic: compare the block's exported response
   (below) with your reference and adjust.
4. **`radiating_fraction`**: 0.5 is a typical paper cone. Lower it for a speaker that is much
   darker off axis than on axis.
5. **Load it.** `block.loadDescription` with the file on a cab.model block: the loader names the
   field if anything is wrong, otherwise the speaker becomes the block's Custom speaker and is
   selected. The text is saved with the block in presets.

```json
{"command": "chain.addBlock", "args": {"kind": "cab.model"}}
{"command": "block.loadDescription", "args": {"blockId": "...", "file": "/path/to/my-speaker.json"}}
```

Describe speakers you own or have data for, write your own numbers, and say in `metadata.licence`
under what terms you share the file. Do not copy another product's cab data, impulse responses or
models into a description, and do not name a description after a product you do not make.

## The cabinets

| Size | Speakers | Baffle (w x h) | Depth | Net volume per speaker (closed) | Speaker centres |
|---|---|---|---|---|---|
| 1x12 | 1 | 0.50 x 0.45 m | 0.25 m | 40 l | centre |
| 2x12 | 2 | 0.74 x 0.48 m | 0.27 m | 33 l | +-0.17 m across |
| 4x12 | 4 | 0.76 x 0.76 m | 0.36 m | 28 l | +-0.17 m across and up |
| 1x10 | 1 | 0.42 x 0.38 m | 0.22 m | 24 l | centre |
| 2x10 | 2 | 0.60 x 0.40 m | 0.24 m | 20 l | +-0.14 m across |

The cab stands on the floor on 3 cm feet. In a 2x12 or 4x12 the mic is on the left (upper left)
speaker and the position moves it towards the box's centre, so with distance the other speakers
come in, in step at low frequencies (the big bottom of a 4x12 heard from a metre) and with comb
filtering higher up.

## Exporting a cab as an IR

`block.exportImpulse` writes the response a cab.model block plays now as a mono WAV, at any sample
rate (default 48 kHz), 16 or 24 bit (peak at -1 dBFS) or 32 bit float (the block's own level):

```json
{"command": "block.exportImpulse", "args": {"blockId": "...", "file": "/path/to/cab.wav", "sampleRate": 48000, "bits": 24}}
```

Without the app, `PedaleiraRender --cab-ir` loads a rig and writes the response of its first
cab.model block, 24 bit at the render rate (`--rate`, default 48 kHz); `--in` and `--out` may be
left out:

```
PedaleiraRender --preset my-rig.json --cab-ir my-cab.wav --rate 48000
```

## An amp's default cab

An amp description may name the cab it is meant for, in an optional `default_cab` section
(`docs/amp-framework.md`): the speaker by name and the settings of a cab.model block. It is a
suggestion for whoever builds a rig around the amp; nothing of the amp's own sound depends on it.

```json
"default_cab": {"speaker": "Granite", "size": "4x12", "back": "closed", "mic": "dynamic",
                "position_percent": 35, "distance_cm": 2.5}
```

## Real time

- **Off the audio thread.** One builder thread serves every cab block in the process
  (`dsp/CabIrWorker.h`); it exists while a cab block does. About every 10 ms it compares each
  block's parameters with those of its live response and, when they differ, computes the new
  response (about 8 ms at 48 kHz on a busy 4-core build machine) and loads it into the block's second convolver, which is idle.
  A knob turned quickly gives a few responses in a row; each new one waits for the previous fade to
  end, and the builder always builds the latest settings.
- **Click-free.** The audio thread then runs both convolvers: the new one silent for 45 ms while
  its history fills (all of the response but its faint end, so it then plays what it would have,
  had it always been there), then an equal-gain crossfade over 25 ms. The old convolver goes back to the builder. The hand-off is two
  atomics: no allocation, lock or wait on the audio thread.
- **Prepared synchronously.** A prepare (the host starting, a restart) builds the response for the
  settings of that moment on the calling thread, so a render made after a prepare is repeatable,
  sample for sample.
- **Latency 0.** The convolution is JUCE's uniform partitioned engine with zero latency, prepared
  at the base block size capped at 256 like the IR blocks (`IrBlockProcessor.h`).
- **CPU.** One 85 ms convolution per channel: about 1.1 % of a core for a stereo block at 48 kHz
  in 128-sample blocks, 2.7 % for the 70 ms of a swap, measured on a busy build machine
  (`CabModelCost` and the conformance test print the numbers).

## What is not modelled

- The speaker's impedance seen by a tube power amp (the resonance and coil rise the amp's output
  impedance turns into a bass bump and a treble lift): that is the amp's depth and presence
  (`docs/amp-framework.md`).
- Anything non-linear: cone and suspension distortion, power compression, a speaker that breaks up
  differently when driven hard.
- Box panel resonances, the standing waves inside a box, the cab's stand or a tilted cab.
- A second mic, a stereo pair, the phase of a mic against the dry signal.
- Reverb beyond the early reflections; any room but the one described above.

## Licences

The code is under the licence of this repository (MIT). The shipped speaker descriptions are our
own voicings, under MIT; they describe no product. The equations come from the published
literature cited above.

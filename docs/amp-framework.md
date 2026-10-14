# The amp framework

How to rebuild a guitar amp you own, from its schematic, as data.

An amp in Pedaleira is a **description**: a JSON file that lists the parts of the circuit (the
tubes, the resistors and capacitors around each of them, the pots, the tone stack, the power
amp and its supply) and the front panel (which knob moves which part). The `amp.custom` block
loads a description and builds the amp it describes from a set of small, tested components.
Our first amp, High Gain (`amp.highgain`), is one such description, `resources/amps/high-gain.json`;
it has no code of its own.

```
resources/amps/high-gain.json        the high gain amp, as data (linked into the binary)
resources/amps/high-gain-wdf.json    the same amp with the wave digital filter solver
docs/amp-description.schema.json     JSON Schema of the format
plugin/include/amp/                  the components (namespace amp, plain C++, no JUCE)
plugin/src/effects/amp/              the amp kinds (amp.highgain, amp.custom) and the loader
```

Contents: [the signal flow](#the-signal-flow), [the components](#the-components-and-what-each-models),
[the two stage models](#the-two-stage-models), [the description format](#the-description-format),
[the high gain amp, annotated](#the-high-gain-amp-annotated), [loading a description](#loading-a-description),
[from a schematic to a description](#from-a-schematic-to-a-description),
[measuring an amp and fitting the description](#measuring-an-amp-or-a-capture-and-fitting-the-description),
[the fitted amps](#the-fitted-amps), [licences](#licences), [what is not modelled](#what-is-not-modelled).

## The signal flow

```
guitar -> input coupling high-pass -> grid stopper low-pass
       -> stage 1 (input stage)  [cathode network -> triode -> plate load] -> gain pot (bright cap) -> coupling
       -> stage 2                [cathode network -> triode -> plate load] -> coupling
       -> stage 3 ... (up to 6 stages, one gain pot, a coupling after every stage but the last)
       -> passive tone stack (treble, middle, bass) -> master -> tone cut (when the amp has one)
       -> power amp (push-pull or solid-state saturation, global feedback with presence and depth, supply sag)
       -> output transformer corner -> level
```

Everything from the first grid to the power amp runs at 4 times the host rate, through the same
half-band oversampler the drive blocks use, because the stages clip hard. Knobs glide over 20 ms
and take effect on a fixed grid of 16 samples, so the sound does not depend on the block size.

## The components and what each models

All of them are in `plugin/include/amp` (namespace `amp`), take component values in ohms,
farads, henries and volts, derive their corners and gains from them, allocate nothing after
`prepare`, and are tested alone in `test/src/effects/amp_components_tests.cpp` and
`amp_wdf_tests.cpp`.

| Component | What it models | Method |
|---|---|---|
| `Filters.h`: `OnePole`, `Svf` | A 6 dB per octave low-pass or high-pass; a 12 dB per octave state variable filter with low, band and high outputs. | Exponential one-pole; the trapezoidal (TPT) state variable filter, V. Zavalishin, *The Art of VA Filter Design*. |
| `FastMath.h` | Reciprocal and reciprocal square root, 23 bits, without the latency of a divide. | The hardware estimate plus one Newton-Raphson step. |
| `TriodeStage.h`: `TriodeStage`, `TriodeCircuit` | One common-cathode triode stage as a static, asymmetric waveshaper: grid conduction clips the positive swing at the bias voltage, cutoff clips the negative swing later; a grid-leak bias shift that follows the drive; the stage gain mu Ra / (Ra + rp). | The curve follows the plate characteristics of N. Koren, "Improved vacuum tube models for SPICE simulations" (2003) in the shape x / sqrt(1 + x^2); grid conduction and bias shift as surveyed by J. Pakarinen and D. T. Yeh, "A review of digital techniques for modeling vacuum-tube guitar amplifiers", *Computer Music Journal* 33(2), 2009; first-order antiderivative anti-aliasing, J. D. Parker, V. Zavalishin and E. Le Bivic, DAFx-16 (2016), and S. Bilbao, F. Esqueda, J. D. Parker and V. Valimaki, "Antiderivative antialiasing for stateful systems" (2017). |
| `CouplingFilters.h`: `CouplingNetwork`, `CathodeShelf`, `GainPot`, `ToneCut` | The coupling capacitor into a series resistor and a grid leak (a high-pass with a divider); the cathode resistor with its bypass capacitor (a low shelf: less gain below 1 / (2 pi Rk Ck), from cathode feedback); the gain pot between two stages, an audio taper with a floor resistor, loaded by the next grid leak, with a bright capacitor across its upper part (H(s) = H0 (1 + s tz) / (1 + s tp)); the tone cut of a power amp without global feedback, a pot and a capacitor that shunt the drive of the output tubes (H(s) = (1 + s Rc C) / (1 + s (Rsrc + Rc) C)). | Textbook common-cathode stage with cathode bypass; the pot and bright cap, and the tone cut, as exact first-order networks. |
| `ToneStack.h` | The passive treble, middle and bass tone stack of the Fender / Marshall family, with the interaction between the knobs and the mid dip. | Closed form third-order transfer function, D. T. Yeh and J. O. Smith, "Discretization of the '59 Fender Bassman Tone Stack", DAFx-06 (2006), re-derived from the netlist (see the correction below); bilinear transform in double precision. |
| `WdfToneStack.h` | The same network, solved as a wave digital filter with an R-type adaptor. An independent reference for `ToneStack.h` (it shares no algebra with it), and a complete alternative implementation. | A. Fettweis, "Wave digital filters: theory and practice", *Proc. IEEE* 74(2), 1986; R-type adaptors for arbitrary topologies, K. J. Werner, J. O. Smith and J. S. Abel, DAFx-15 (2015); the library chowdsp_wdf, J. Chowdhury, arXiv:2210.12554. |
| `PowerAmp.h` | A push-pull pentode power stage: soft saturation scaled by the supply headroom, class AB crossover character, global negative feedback through the output transformer's low-pass, Presence (removes high-frequency feedback) and Depth (removes feedback in the speaker's resonance band). Or a solid-state output stage: linear up to its rail and clipped flat there (with antiderivative anti-aliasing), in the same feedback loop. | Feedback structure as in Pakarinen and Yeh (2009); the output transformer is two poles in the feedback path, the screen grid and the phase inverter are not modelled. |
| `SagModel.h` | Supply sag: the high-voltage rail droops under load and recovers over tens of milliseconds, so loud passages compress and bloom. | The usual lumped rectifier-and-reservoir description (Pakarinen and Yeh): depth = source resistance x full load current / B+, recovery = source resistance x reservoir capacitance. |
| `GainStages.h`: `GainStageSpec`, `ClassicGainStage` | The whole of one gain stage as component values, and its classic realisation: the pieces above chained. | See "The two stage models". |
| `WdfTriodeStage.h`: `WdfGainStage` and the plate, cathode and pot trees | The same stage with the networks around the tube solved as wave digital filter trees and the tube as a nonlinear root. | Wave digital filters as above; wave digital triode stages go back to M. Karjalainen and J. Pakarinen, "Wave digital simulation of a vacuum-tube amplifier", ICASSP 2006; the implicit tube equation is solved per sample by Newton's method. The tree library is chowdsp_wdf. |
| `AmpDescription.h` and `AmpDescription.cpp` | The description as a structure, and the loader with its checks. | nlohmann json (already a dependency of the NAM core). |
| `AmpCircuit.h` | One channel of an amp built from a description: the chain above, and the control values (`ControlFrame`) the knobs turn into. | |

**A correction to the tone stack.** Checking `ToneStack.h` against `WdfToneStack.h` found that the
third-order terms (s^3) of its numerator and denominator did not match the network: they gave complex
poles at about 2 knob settings in 100 (52 and 65 of 3000 random stacks), which a passive RC network
cannot have, and a response up to 1 dB off the circuit (0.99 dB at treble 0.9, middle 0.9, bass 0.1).
The terms were re-derived from the netlist and now agree with the WDF to 0.006 dB; the sound of
`amp.highgain` moved by about -50 dB of error power against the old one (largest sample difference 2.4 mV on
a 0.2 peak).

## The two stage models

A gain stage is the triode and everything that hangs on it. Two models of it exist, with the same
interface (`GainStages.h`), chosen per amp by `model.stage_solver` in the description.

| | `classic` | `wdf` |
|---|---|---|
| Tube | Static curve driven by the grid voltage (with anti-aliasing). | Implicit: the tube equation `rp i = mu c(vgk) + vpk` with the same curve `c`, solved per sample. |
| Plate load, plate capacitance, coupling into the next grid | A one-pole low-pass at Ra \|\| rp and Cp, then a one-pole high-pass and a divider. The next grid does not load the plate. | One WDF tree `Ra \|\| Cp \|\| (Cc + Rs + Rg)`; the next grid network loads the plate as it does in the circuit. |
| Cathode resistor and bypass capacitor | A gain shelf ahead of the curve; its transition sits lower in frequency than the real one by the feedback ratio. | A WDF tree `Rk \|\| Ck`; true cathode feedback through the tube equation (it also reduces distortion at low frequencies). An ideally bypassed cathode needs no iteration. |
| Gain pot and bright cap | One-pole filters of the exact first-order network, loaded by the next grid leak alone. | One WDF tree: source resistance, upper pot section with the bright cap, lower section in parallel with the coupling to the next grid. |
| Grid conduction | An attack / release bias shift driven by the grid voltage. | The same element (a grid diode at the next grid would be a second nonlinearity inside a one-root tree). |
| Anti-aliasing | Antiderivative anti-aliasing of the curve plus the 4x oversampling. | The 4x oversampling only (the antiderivative does not carry over to an implicit solve). |
| Cost | The reference. | About 2 times the classic amp for one lane (3.3 % against 2.0 % of a core for the whole amp, 128-sample blocks at 48 kHz, measured on a shared machine). |
| Mirrored stereo | A mono source in a stereo block runs one lane. | Both lanes run (the trees are not copyable). |

The classic model is what `amp.highgain` ships with, so its sound does not change; the wdf model
is the more faithful one and is the one to use when the circuit matters more than the cost. Where
they differ on small signals, and by how much, is measured in `amp_wdf_tests.cpp`:
the plate network with the coupling to the next grid, 0.64 dB lower (the loading); the cathode shelf, up to 1.7 dB around 200 Hz (the shelf transition); the gain pot, within 0.3 dB from 100 Hz to 8 kHz, up to 1.8 dB at the top with the bright cap at low gain (the one-pole form of the bright shelf); the four-stage preamp, within 2.9 dB from 100 Hz to 6.4 kHz at three gain settings. At drive the THD of one stage agrees to 0.01 points above 0.5 V (16.9 % against 16.9 % at 1.5 V). The whole wdf amp plays 0.3 dB lower than the classic one at the defaults.

The linear networks of the wdf model are checked against their analogue transfer functions
(computed in the test from the component values) to within 0.5 dB from 20 Hz to 10 kHz:
plate load with the coupling into the next grid 0.03 dB, cathode network 0.0001 dB, gain pot with the bright capacitor 0.05 dB, the tone stack 0.006 dB.

## The description format

A description is one JSON file, format `pedaleira-amp`, version 1. The loader
(`AmpDescription.cpp`) checks every field's type and range, rejects unknown fields (a misspelt
field would otherwise silently keep its default), and says where the fault is:
`stages[1].coupling.grid_leak_ohm: missing`. `docs/amp-description.schema.json` is the JSON
Schema of the same format, for an editor; the loader is the authority.

**Units** are in the field names: `_ohm`, `_f` (farads), `_h` (henries), `_v`, `_a`, `_s`, `_hz`.

**Provenance.** Every number can be written bare, or as an object that says where it came from:

```json
"plate_capacitance_f": { "value": 295e-12, "provenance": "fitted", "note": "wiring plus the next Miller capacitance" }
```

`provenance` is `"schematic"` (read off the schematic or a datasheet), `"measured"` (measured on
the real amp or part) or `"fitted"` (no component behind it; chosen to make the model match a
measurement or a capture). A bare number is `"schematic"`, except where the field is fitted by
nature (the grid conduction model, the power stage knee and crossover, the transformer corners,
the sag attack, everything in `calibration`), which are `"fitted"`. The loader keeps the source
and the note of every value that has an explicit one and of every fitted value
(`AmpDescription::notes`), so a fitting tool can list what is free to move.

**Metadata** names the amp and says under which licence the description itself is shared
(`metadata.licence` is required; see [Licences](#licences)).

### Sections

#### `metadata`

Who made the description and under what terms.

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `name` | string |  |  |  | required | The amp's name; shown as the block's name. |
| `author` | string |  |  |  |  | Who wrote the description. |
| `licence` | string |  |  |  | required | The licence of this description file (an SPDX identifier such as "MIT", "CC0-1.0", "CC-BY-4.0", or a name). The description is data you own; say what others may do with it. |
| `summary` | string |  |  |  |  | A sentence or two about the amp and what the description is based on. |
| `references` | array of strings |  |  |  |  | Where the values come from: schematic names, papers, measurements. |

#### `model`

How the networks around each tube are solved.

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `stage_solver` | string |  |  | classic |  | "classic": one-pole filters and a static waveshaper (cheap, the shipped high-gain amp). "wdf": wave digital filter trees with the tube as a nonlinear root (slower, closer to the circuit). |

#### `input`

The input jack up to the first grid.

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `coupling_capacitor_f` | number | F | 1e-12 to 0.001 | 6.8e-9 |  | Input coupling capacitor. (default provenance: schematic) |
| `grid_leak_ohm` | number | ohm | 1000 to 1e+8 | 1e+6 |  | The first grid leak to ground. (default provenance: schematic) |
| `grid_stopper_ohm` | number | ohm | 0 to 1e+7 | 68000 |  | The grid stopper resistor in series with the first grid. (default provenance: schematic) |
| `miller_f` | number | F | 1e-13 to 1e-6 | 1.6e-10 |  | Capacitance the stopper works against (the first stage's Miller capacitance). (default provenance: schematic) |
| `source_ohm` | number | ohm | 0 to 1e+6 | 10000 |  | The guitar's source impedance; sets how fast the first coupling capacitor charges when the grid conducts. (default provenance: fitted) |

#### `stages[]`

One entry per gain stage, in signal order, 1 to 6 of them. The first is the input stage. Every stage but the last has a `coupling`; one stage may have a `gain_pot` (between its plate and its coupling).

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `name` | string |  |  |  |  | Label for the stage ("V2"). |
| `tube` | name or object |  |  |  | required | "12AX7" (or "ECC83"), "12AT7" ("ECC81"), "12AU7" ("ECC82"), or {"mu": ..., "rp_ohm": ...}. Datasheet small-signal constants at a typical operating point. |
| `plate_load_ohm` | number | ohm | 1000 to 1e+7 |  | required | The plate resistor Ra to the supply. (default provenance: schematic) |
| `plate_capacitance_f` | number | F | 0 to 1e-6 | 0 |  | Capacitance on the plate: wiring plus the next stage's Miller capacitance. With the plate impedance it sets the stage's high-frequency roll-off. (default provenance: schematic) |
| `cathode` | object |  |  |  | required | The cathode network. |
| `cathode.resistor_ohm` | number | ohm | 0 to 1e+6 |  | required | The cathode resistor Rk. With the bias current it sets the bias voltage, which sets where the curve clips. (default provenance: schematic) |
| `cathode.bias_current_a` | number | A | 1e-6 to 1 |  | required | The plate current at rest. (default provenance: schematic) |
| `cathode.bypass_capacitor_f` | number or name |  |  | ideal |  | The cathode bypass capacitor in farads, or "ideal" (a short for audio, the default), or "none" (unbypassed). |
| `grid` | object |  |  |  |  | The grid model: catalogue and fitted values. |
| `grid.cutoff_v` | number | V | 0.1 to 200 | 3.5 |  | Grid voltage that cuts the plate current off. Defaults to 3.5 for a 12AX7 and is required for any other tube. (default provenance: schematic) |
| `grid.contact_potential_v` | number | V | 0 to 5 | 0.3 |  | Grid conduction starts this far below zero grid-cathode volts. (default provenance: fitted) |
| `grid.conduction_ohm` | number | ohm | 10 to 1e+6 | 2000 |  | Resistance of the conducting grid-cathode diode. (default provenance: fitted) |
| `grid.max_bias_shift_v` | number | V | 0.1 to 100 | 10 |  | Ceiling of the grid-conduction bias shift. (default provenance: fitted) |
| `coupling` | object |  |  |  |  | The network to the next grid. Not allowed on the last stage. |
| `coupling.capacitance_f` | number | F | 1e-12 to 0.001 |  |  | The coupling capacitor to the next grid; leave it out when every voicing gives it. (default provenance: schematic) |
| `coupling.series_ohm` | number | ohm | 0 to 1e+8 | 0 |  | A series resistor into the next grid. (default provenance: schematic) |
| `coupling.grid_leak_ohm` | number | ohm | 100 to 1e+8 |  | required | The next grid leak to ground. (default provenance: schematic) |
| `gain_pot` | object |  |  |  |  | The gain pot with its bright capacitor, between this stage's plate and its coupling. |
| `gain_pot.resistance_ohm` | number | ohm | 1000 to 1e+8 |  | required | Total resistance of the pot. (default provenance: schematic) |
| `gain_pot.floor_ohm` | number | ohm | 0 to 1e+7 | 0 |  | A fixed resistor between the pot and ground, so the control does not reach silence. (default provenance: schematic) |
| `gain_pot.source_ohm` | number | ohm | 0 to 1e+7 |  |  | What drives the pot: the previous plate load. Defaults to the stage's plate_load_ohm. (default provenance: schematic) |
| `gain_pot.load_ohm` | number | ohm | 1000 to 1e+9 |  |  | What loads the wiper (classic solver only; the wdf solver uses the real coupling network). Defaults to the coupling grid leak. (default provenance: schematic) |
| `gain_pot.taper` | number |  | 0.2 to 10 | 3.3 |  | Resistance law of the pot: the resistance below the wiper is R * position ^ taper (3.3 is about 10 percent at half rotation, an audio taper). (default provenance: schematic) |
| `gain_pot.bright_capacitor_f` | number | F | 0 to 1e-6 | 0 |  | The bright capacitor across the upper part of the pot; 0 for none. (default provenance: schematic) |

#### `voicings[]`

1 to 3 voicings: the interstage capacitors and the supply the voicing switch selects.

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `name` | string |  |  |  | required | Shown as the voicing's name. |
| `coupling_capacitors_f` | array |  |  |  |  | One capacitance per link (stage k to stage k + 1), or null to use the stage's own `coupling.capacitance_f`. Recorded as fitted values. |
| `supply` | object |  |  |  |  | Overrides of `power_amp.supply` for this voicing; anything left out is taken from there. |
| `supply.b_plus_v` | number | V | 5 to 2000 |  |  | Unloaded high-voltage rail. (default provenance: schematic) |
| `supply.source_ohm` | number | ohm | 0 to 100000 |  |  | Rectifier plus transformer winding resistance. (default provenance: schematic) |
| `supply.full_load_a` | number | A | 0.0001 to 20 |  |  | Supply current at full output. (default provenance: schematic) |
| `supply.reservoir_f` | number | F | 1e-7 to 1 |  |  | The reservoir (filter) capacitor. (default provenance: schematic) |
| `supply.attack_s` | number | s | 0.0001 to 5 |  |  | How fast the load builds (depends on the music, not the supply). (default provenance: fitted) |
| `supply.rectifier` | string |  |  |  |  | "solid_state", "tube" or "none": a label, the source resistance already contains the effect. |

#### `tone_stack`

The passive tone stack: treble, middle and bass pots with a slope resistor (the Fender / Marshall TMB), solved in closed form (Yeh and Smith).

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `type` | string |  |  | tmb |  | "tmb". |
| `treble_pot_ohm` | number | ohm | 1000 to 1e+8 |  | required | R1. (default provenance: schematic) |
| `bass_pot_ohm` | number | ohm | 1000 to 1e+8 |  | required | R2, used as a variable resistor. (default provenance: schematic) |
| `middle_pot_ohm` | number | ohm | 100 to 1e+8 |  | required | R3. (default provenance: schematic) |
| `slope_ohm` | number | ohm | 100 to 1e+8 |  | required | R4, the slope resistor. (default provenance: schematic) |
| `treble_cap_f` | number | F | 1e-12 to 1e-5 |  | required | C1. (default provenance: schematic) |
| `bass_cap_f` | number | F | 1e-10 to 0.0001 |  | required | C2. (default provenance: schematic) |
| `middle_cap_f` | number | F | 1e-10 to 0.0001 |  | required | C3. (default provenance: schematic) |

#### `effects_loop`

A placeholder: the loop is read and kept, the signal does not pass through one.

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `present` | true / false |  |  | False |  | Whether the amp has a loop. |
| `note` | string |  |  |  |  | Free text. |

#### `power_amp`

The output stage.

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `tube` | string |  |  | EL34 |  | A label for the output tubes. |
| `topology` | string |  |  | push_pull_ab |  | "push_pull_ab" has crossover character; "push_pull_a" has none; "solid_state" has none and is linear up to its rail, where it clips flat (a transistor power amp; the knee and the crossover are not used). |
| `open_loop_gain` | number |  | 1 to 1000 | 20 |  | Forward gain of the stage before its saturation. (default provenance: fitted) |
| `feedback` | object |  |  |  |  | The global negative feedback network. |
| `feedback.series_ohm` | number | ohm | 1 to 1e+7 | 4000 |  | The feedback resistor from the output back. (default provenance: schematic) |
| `feedback.shunt_ohm` | number | ohm | 1 to 1e+7 | 1000 |  | The shunt resistor it divides against: beta = shunt / (shunt + series). (default provenance: schematic) |
| `presence` | object |  |  |  |  | The presence network: removes high-frequency feedback. |
| `presence.pot_ohm` | number | ohm | 0 to 1e+7 | 5000 |  | The presence pot. (default provenance: schematic) |
| `presence.min_ohm` | number | ohm | 0.1 to 1e+7 | 220 |  | Resistance left with the pot turned fully up. (default provenance: schematic) |
| `presence.capacitor_f` | number | F | 1e-12 to 0.001 | 4.7e-8 |  | The capacitor in series with it, across the shunt resistor. (default provenance: schematic) |
| `depth` | object |  |  |  |  | The depth (resonance) network. |
| `depth.inductance_h` | number | H | 0.0001 to 100 | 0.12 |  | Resonance branch inductance (the speaker impedance peak). (default provenance: schematic) |
| `depth.capacitance_f` | number | F | 1e-9 to 1 | 2.1e-5 |  | Its capacitance. (default provenance: schematic) |
| `depth.resistance_ohm` | number | ohm | 0.1 to 100000 | 63 |  | Its resistance: sets the width. (default provenance: schematic) |
| `depth.max_cut` | number |  | 0 to 1 | 0.85 |  | The largest fraction of that band's feedback removed at full depth. (default provenance: fitted) |
| `tone_cut` | object |  |  |  |  | A tone cut ahead of the output tubes: a pot (plus a fixed resistor) in series with a capacitor that shunts the drive, which comes from `source_ohm`. Turned up it takes the highs away. Left out: no tone cut. The `presence` control can drive it (see the panel). |
| `tone_cut.source_ohm` | number | ohm | 100 to 1e+7 |  | required | What drives the network (the phase inverter's plate). (default provenance: schematic) |
| `tone_cut.pot_ohm` | number | ohm | 0 to 1e+7 |  | required | The tone cut pot. (default provenance: schematic) |
| `tone_cut.min_ohm` | number | ohm | 0 to 1e+7 | 0 |  | Resistance left in the branch with the cut fully up. (default provenance: schematic) |
| `tone_cut.capacitor_f` | number | F | 1e-12 to 0.0001 |  | required | The capacitor in series with the pot. (default provenance: schematic) |
| `transformer` | object |  |  |  |  | Output transformer corners. |
| `transformer.high_corner_hz` | number | Hz | 500 to 100000 | 9000 |  | Output transformer high corner (two poles in the feedback path). (default provenance: fitted) |
| `transformer.low_corner_hz` | number | Hz | 1 to 500 | 12 |  | Output transformer low corner (the output DC blocker). (default provenance: fitted) |
| `knee_mix` | number |  | 0 to 1 | 0 |  | 0 is a soft knee, 1 a sharp one. (default provenance: fitted) |
| `crossover` | object |  |  |  |  | Class AB crossover character; ignored (ratio 1) for push_pull_a. |
| `crossover.ratio` | number |  | 0 to 1 | 0.9 |  | Small-signal gain at idle relative to full gain (class AB crossover). (default provenance: fitted) |
| `crossover.width` | number |  | 0.01 to 10 | 0.3 |  | Level over which the notch recovers. (default provenance: fitted) |
| `supply` | object |  |  |  | required | The supply, sag depth = source resistance * full load current / B+, recovery time = source resistance * reservoir. |
| `supply.b_plus_v` | number | V | 5 to 2000 | 450 |  | Unloaded high-voltage rail B+. (default provenance: schematic) |
| `supply.source_ohm` | number | ohm | 0 to 100000 | 250 |  | Rectifier plus transformer winding resistance. (default provenance: schematic) |
| `supply.full_load_a` | number | A | 0.0001 to 20 | 0.3 |  | Supply current at full output. (default provenance: schematic) |
| `supply.reservoir_f` | number | F | 1e-7 to 1 | 0.00022 |  | The reservoir capacitor. (default provenance: schematic) |
| `supply.attack_s` | number | s | 0.0001 to 5 | 0.008 |  | How fast the load builds. (default provenance: fitted) |
| `supply.rectifier` | string |  |  | tube |  | "solid_state", "tube" or "none". |

#### `calibration`

Values with no component behind them; they tie the model to levels.

| Field | Type | Unit | Range | Default | | Meaning |
|---|---|---|---|---|---|---|
| `input_full_scale_v` | number | V | 0.001 to 1000 | 2 |  | Digital full scale at the first grid, in volts. (default provenance: fitted) |
| `power_drive_v` | number | V | 0.01 to 10000 | 40 |  | Volts at the tone stack output (after the master) that drive the power stage to full swing; stands for the recovery stage and phase inverter. (default provenance: fitted) |
| `coupling_gain_rise` | number |  | 0 to 20 | 1.8 |  | How much the interstage corners rise with gain (factor 1 + rise at full gain): the gain-dependent tightening of the low end. (default provenance: fitted) |
| `output_trim` | number |  | 0.0001 to 100 | 0.2 |  | Scale from the power stage's swing to digital. (default provenance: fitted) |
| `control_smoothing_s` | number | s | 0.0001 to 5 | 0.02 |  | How fast the knobs glide. (default provenance: fitted) |


### The panel

`panel` says which knobs the amp has and what each moves. The control ids are fixed, they are the
parameter ids of the block: `gain`, `bass`, `middle`, `treble`, `presence`, `depth`, `master`,
`bright` (a switch), `voicing` (a choice). A control that is left out is not on the amp and is held
at the middle of its travel. Without a `panel` section every control the parts allow is present,
linear, over its full travel (the master with taper 2). An amp with a tone cut and no presence
control gives its tone cut knob the `presence` slot (`"presence": {"label": "Tone Cut", "target":
"power_amp.tone_cut"}`), so the block keeps its ten parameters; a tone cut no knob drives is held at
the middle of its travel.

| Control | `target` (must be exactly this) | What it moves |
|---|---|---|
| `gain` | `stages[N].gain_pot` (N the stage that has the pot) | The pot's wiper position. Needs a `gain_pot`. |
| `bright` | `stages[N].gain_pot.bright_capacitor_f` | The bright capacitor in or out. |
| `bass` | `tone_stack.bass_pot_ohm` | Position `l` of the bass pot in the tone stack. |
| `middle` | `tone_stack.middle_pot_ohm` | Position `m`. |
| `treble` | `tone_stack.treble_pot_ohm` | Position `t`. |
| `presence` | `power_amp.presence` | The presence pot (0 = most feedback, 1 = most high frequency feedback removed). |
| `presence` | `power_amp.tone_cut` | On an amp with a `tone_cut` (and no presence control): the tone cut (0 = least cut, 1 = most); the presence network is then held at the middle of its travel. |
| `depth` | `power_amp.depth` | How much of the resonance band's feedback is removed. |
| `master` | `master` | The master volume between the tone stack and the power amp: gain = position / `calibration.power_drive_v` volts. |
| `voicing` | `voicings` | Which voicing is selected. |

`taper` and `range` say how the knob is wired to the part: position = low + (high - low) *
knob ^ taper, with the knob 0 to 1. A bass pot with a log taper is `"taper": 2.0`; a gain pot
that only uses the upper half of its travel is `"range": [0.5, 1.0]`. This is the law between the
knob and the pot's rotation; the gain pot's own resistance law is `gain_pot.taper`.

The amp.custom block always has the same ten parameters (`gain`, `bass`, `middle`, `treble`,
`presence`, `depth`, `master`, `levelDb`, `bright`, `voicing`), 0 to 100 percent, so a preset,
a scene or an expression pedal assigned to one keeps working when the description behind it
changes. `levelDb` is the block's output level and is not part of the description. The voicing
choice has three slots ("Voicing 1" to "Voicing 3"); a description with fewer uses the last one
for the slots it does not have.

## The high gain amp, annotated

`resources/amps/high-gain.json`, section by section. JSON has no comments, so they are here.

**Header and metadata.**

```json
{
  "format": "pedaleira-amp",
  "version": 1,
  "metadata": {
    "name": "High Gain",
    "author": "Pedaleira",
    "licence": "MIT",
    "summary": "A modern four-stage high gain preamp ... A typical topology of the genre, not the schematic of one product ...",
    "references": [ "D. T. Yeh and J. O. Smith, ... DAFx-06, 2006", "J. Pakarinen and D. T. Yeh, ... 2009" ]
  },
  "model": { "stage_solver": "classic" },
```

`format` and `version` are fixed. `metadata.name` becomes the block's name, `licence` is the licence of
this file. This amp is a typical topology of its genre and not the schematic of one product, and the
summary says so: be as plain about what yours is based on. `model.stage_solver` picks the stage
model (`classic` here; `high-gain-wdf.json` is this file with `"wdf"`).

**The input.**

```json
  "input": {
    "coupling_capacitor_f": 6.8e-9,
    "grid_leak_ohm": 1.0e6,
    "grid_stopper_ohm": 68000,
    "miller_f": 160e-12,
    "source_ohm": { "value": 10000, "provenance": "fitted", "note": "assumed pickup impedance ..." }
  },
```

A 6.8 nF capacitor into a 1 megohm grid leak is a high-pass at 1 / (2 pi 6.8 nF 1 M) = 23 Hz. The 68k
grid stopper against 160 pF of Miller capacitance is a low-pass at 14.6 kHz. The guitar's source
impedance only matters for how fast the first coupling capacitor charges when the grid conducts, so
it is a fitted value with a note.

**The first stage, with the gain pot.**

```json
  "stages": [
    {
      "name": "V1 input stage",
      "tube": "12AX7",
      "plate_load_ohm": 100000,
      "plate_capacitance_f": { "value": 295e-12, "provenance": "fitted", "note": "..." },
      "cathode": { "resistor_ohm": 1500, "bias_current_a": 0.001, "bypass_capacitor_f": 0.68e-6 },
      "grid": { "cutoff_v": 3.5, "contact_potential_v": 0.3, "conduction_ohm": 2000, "max_bias_shift_v": 10 },
      "coupling": { "series_ohm": 0, "grid_leak_ohm": 470000 },
      "gain_pot": {
        "resistance_ohm": 1.0e6, "floor_ohm": 22000, "source_ohm": 100000, "load_ohm": 470000,
        "taper": 3.3, "bright_capacitor_f": 100e-12
      }
    },
```

A 12AX7 (mu 100, rp 62.5k) with a 100k plate resistor: the stage gain with the cathode bypassed is
mu Ra / (Ra + rp) = 61.5, 35.8 dB. The cathode resistor of 1.5k at 1 mA is 1.5 V of bias, which is
where the grid starts to conduct on the positive swing (`posLimit`); the grid is cut off 3.5 V
below (`cutoff_v`), so the curve clips later on the negative swing: the asymmetry that makes even
harmonics. Bypassed with 0.68 uF the cathode network has its corner at 1 / (2 pi Rk Ck) = 156 Hz; below
it the stage has less gain by the cathode feedback. `grid` holds the catalogue cutoff and the fitted
parameters of grid conduction. `coupling` is the network to the *next* grid: no series resistor,
a 470k grid leak; its capacitor is not here, the voicings give it. `gain_pot` sits between this
stage's plate and that coupling: a 1M audio-taper pot (`taper` 3.3: 10 percent of the resistance at
half rotation) with a 22k fixed resistor below it, driven from the 100k plate load, loaded by the
470k grid leak, with a 100 pF bright capacitor across its upper part.

**The other stages.**

```json
    { "name": "V2 gain stage", "tube": "12AX7", "plate_load_ohm": 100000,
      "plate_capacitance_f": { "value": 345e-12, "provenance": "fitted", "note": "..." },
      "cathode": { "resistor_ohm": 1500, "bias_current_a": 0.001, "bypass_capacitor_f": "ideal" },
      "coupling": { "series_ohm": 470000, "grid_leak_ohm": 25000 } },
    ... V3 the same with a 10k grid leak, V4 without a coupling
```

V2 to V4 are fully bypassed (`"ideal"`: a short for audio). Their couplings put a 470k resistor in
series with a small grid leak, a divider that sets the gain of the link (25k / 495k, then 10k / 480k). The last stage has no `coupling`: its plate goes
to the tone stack.

**Voicings.**

```json
  "voicings": [
    { "name": "Tight",   "coupling_capacitors_f": [4.7e-9, 2.7e-9, 2.0e-9],
      "supply": { "source_ohm": { "value": 100, "provenance": "fitted", "note": "a stiff solid-state supply" }, "rectifier": "solid_state" } },
    { "name": "Classic", "coupling_capacitors_f": [8.2e-9, 4.7e-9, 3.9e-9], "supply": { "source_ohm": ..., "rectifier": "tube" } },
    { "name": "Loose",   "coupling_capacitors_f": [15e-9, 10e-9, 8.2e-9],  "supply": { ... } }
  ],
  "default_voicing": 1,
```

One capacitor per link (stage 1 to 2, 2 to 3, 3 to 4). Tight uses small capacitors (corners near
72, 119 and 166 Hz) and a stiff supply (100 ohm of source resistance), Loose large ones (near 23,
32 and 40 Hz) and a soft supply (450 ohm). Anything a voicing does not give is taken from
`power_amp.supply`. `default_voicing` is the one a block starts on (Classic).

**The tone stack.**

```json
  "tone_stack": { "type": "tmb", "treble_pot_ohm": 220000, "bass_pot_ohm": 1.0e6, "middle_pot_ohm": 22000,
                  "slope_ohm": 39000, "treble_cap_f": 400e-12, "bass_cap_f": 22e-9, "middle_cap_f": 22e-9 },
  "effects_loop": { "present": false, "note": "placeholder: ..." },
```

R1 220k (treble), R2 1M (bass), R3 22k (middle), R4 39k (slope), C1 400 pF, C2 and C3 22 nF. The
network is described in `WdfToneStack.h`.

**The power amp.**

```json
  "power_amp": {
    "tube": "EL34", "topology": "push_pull_ab", "open_loop_gain": 20,
    "feedback": { "series_ohm": 4000, "shunt_ohm": 1000 },
    "presence": { "pot_ohm": 5000, "min_ohm": 220, "capacitor_f": 47e-9 },
    "depth": { "inductance_h": 0.12, "capacitance_f": 21e-6, "resistance_ohm": 63, "max_cut": 0.85 },
    "transformer": { "high_corner_hz": 9000, "low_corner_hz": 12 },
    "knee_mix": 0.0, "crossover": { "ratio": 0.9, "width": 0.3 },
    "supply": { "b_plus_v": 450, "source_ohm": 250, "full_load_a": 0.3, "reservoir_f": 220e-6, "attack_s": 0.008, "rectifier": "tube" }
  },
```

The feedback divider is beta = 1000 / (1000 + 4000) = 0.2 with an open-loop gain of 20, so a loop
gain of 4. Presence is a 5k pot with 220 ohm left at the top and a 47 nF capacitor across the shunt resistor:
turned up (220 ohm) the feedback above 1 / (2 pi 47 nF 1.22k) = 2.8 kHz falls to 220 / 1220 = 18 percent of
its low-frequency value, so the highs rise; turned down (5220 ohm) 84 percent of it remains above 540 Hz.
Depth is a resonance branch at 1 / (2 pi sqrt(0.12 H 21 uF)) = 100 Hz with a Q of
sqrt(0.12 / 21e-6) / 63 = 1.2, the band whose feedback it removes. The supply: 450 V, 250 ohm and 0.3 A full load is
250 x 0.3 / 450 = 17 percent of the rail lost at full output (the sag depth), recovering with the time
constant 250 ohm x 220 uF = 55 ms.

**Calibration and panel.**

```json
  "calibration": { "input_full_scale_v": 2.0, "power_drive_v": 40, "coupling_gain_rise": 1.8,
                   "output_trim": 0.2, "control_smoothing_s": 0.02 },
  "panel": {
    "gain":   { "label": "Gain",   "target": "stages[0].gain_pot" },
    "bass":   { "label": "Bass",   "target": "tone_stack.bass_pot_ohm", "taper": 2.0 },
    "middle": { "label": "Middle", "target": "tone_stack.middle_pot_ohm" },
    "treble": { "label": "Treble", "target": "tone_stack.treble_pot_ohm" },
    "presence": ..., "depth": ...,
    "master": { "label": "Master", "target": "master", "taper": 2.0 },
    "bright": { "label": "Bright", "target": "stages[0].gain_pot.bright_capacitor_f" },
    "voicing": { "label": "Voicing", "target": "voicings" }
  }
}
```

`calibration` holds the numbers with no component behind them: 2 V at digital full scale at the first
grid (so -18 dBFS is 0.25 V), 40 V at the tone stack output for full power amp swing (standing for the
recovery stage and the phase inverter), the 1.8 rise of the interstage corners at full gain, and
the output scale. The panel wires the nine controls: the bass pot is log (`taper` 2), so bass at 50
percent is position 0.25, and so is the master.


## Loading a description

A description is not a parameter in the block's table (parameters are numbers). Like the model of
an IR or capture block, it is a file the block is given by a command, and like a hosted plugin's
state it travels in the saved preset:

```json
{"command": "chain.addBlock", "args": {"kind": "amp.custom"}}
{"command": "block.loadDescription", "args": {"blockId": "...", "file": "/path/to/my-amp.json"}}
```

`block.loadDescription` reads the file, checks it (the answer is `invalid_args` with the loader's
message, which names the field, and the block is left as it was), gives the block that amp, sets the
voicing to the description's `default_voicing`, and names the block after `metadata.name`. It is one
undo step. A new `amp.custom` block starts as the High Gain description. The text of the
description is saved with the block in presets (a preset does not depend on the file staying
where it was); a block that still has the shipped description saves nothing extra. See
`docs/commands.md`.

A description that is shipped with the program is added by putting the file in `resources/amps` and
re-running CMake (`cmake/AmpFramework.cmake` links every `*.json` there into the binary as
`PedaleiraAmpData`); `plugin/include/effects/amp/BuiltinAmps.h` reads them. A new kind such as
`amp.highgain` is then a small file under `plugin/src/effects/amp` that names its description, as
`HighGainAmp.cpp` does.

## From a schematic to a description

You need the schematic of the amp (yours, or one you are allowed to read), a multimeter for the
voltages the schematic does not print, and the datasheets of the tubes. The steps below follow the
signal.

1. **Count the stages.** Walk from the input jack to the tone stack and count the triode gain
   stages (a 12AX7 holds two). Each becomes one entry of `stages`, in order. Up to six are
   supported. A cathode follower, a long-tailed pair or any other stage that is not a
   common-cathode voltage amplifier is not modelled (see [What is not modelled](#what-is-not-modelled));
   leave it out and absorb its level into `calibration`.
2. **The input network.** The input coupling capacitor and the first grid leak go in `input`
   (`coupling_capacitor_f`, `grid_leak_ohm`), with the grid stopper and the Miller capacitance it
   works against (the stopper times the Miller capacitance is the first low-pass; a 12AX7 at gain 60
   has about 160 pF). `source_ohm` is the guitar: 10k is a fair pickup.
3. **For every stage, read:**
   - the tube (`tube`). For anything that is not a 12AX7, 12AT7 or 12AU7 give `{"mu": ..., "rp_ohm": ...}`
     from the datasheet at the operating point, and the grid voltage that cuts the plate current off
     (`grid.cutoff_v`; about the plate voltage over mu);
   - the plate resistor (`plate_load_ohm`) and the capacitance on the plate (`plate_capacitance_f`:
     wiring, about 5 to 20 pF, plus the next tube's Miller capacitance, Cgp (1 + gain), about 100 to
     400 pF for a 12AX7 driving a stopper);
   - the cathode: `resistor_ohm`, the plate current at rest (`bias_current_a`, the cathode voltage
     divided by the resistor, measured or from the schematic), and the bypass: the capacitor in
     farads, `"ideal"` when it is large enough that its corner is below 20 Hz (the default), or
     `"none"`;
   - the coupling to the next grid: the coupling capacitor (`capacitance_f`, or leave it to the
     voicings), any series resistor in front of the next grid (a mixing or stopper resistor), and the
     next grid leak (`grid_leak_ohm`, required). The last stage has no `coupling`: its plate goes to
     the tone stack.
4. **The gain pot.** The pot between two stages goes into the `gain_pot` of the stage before it:
   `resistance_ohm`, the fixed resistor below it (`floor_ohm`), the bright capacitor across its upper
   part (`bright_capacitor_f`), and `taper`: the exponent of the resistance law. An audio ("A", log)
   taper is about 10 to 20 percent of the resistance at half rotation; the resistance below the
   wiper is R x position^taper, so 3.3 gives 10 percent and 2.3 gives 20 percent. `source_ohm` defaults
   to the previous plate load.
5. **Voicings.** If the amp has switches or jumpers that change the interstage capacitors or the
   supply (a "tight / vintage" switch), each position is a voicing (up to three) with its
   `coupling_capacitors_f` and the `supply` parts that change. An amp with one sound has no
   `voicings` section.
6. **The tone stack.** Check the topology first: a treble pot whose top is fed by a capacitor from
   the input, a bass pot used as a variable resistor and a middle pot with a slope resistor, as in
   the Fender, Marshall and Mesa/Boogie-style circuits. If the schematic matches
   (`WdfToneStack.h` documents the netlist), read the three pot values, the slope resistor and the
   three capacitors. If your stack is a different circuit it is not supported yet (the `WdfToneStack`
   netlist is the place to start).
7. **The power amp.** `tube` and `topology` are labels (class AB or class A push-pull). The global
   feedback is a resistor from the output transformer secondary back to the phase inverter, divided
   against a shunt resistor to ground: `feedback.series_ohm` and `shunt_ohm`. The presence control is a
   pot and a capacitor in series across the shunt resistor (`presence`). The depth (resonance) control
   has no component in most schematics: it stands for the speaker's impedance peak. Read the speaker's
   resonance frequency f0 and Q from its datasheet or an impedance sweep and pick L, C and R with
   f0 = 1 / (2 pi sqrt(L C)) and Q = sqrt(L / C) / R. `transformer.high_corner_hz` and `low_corner_hz`
   are the output transformer's response corners.
8. **The supply.** `b_plus_v` is the unloaded rail, `source_ohm` the rectifier plus transformer
   winding resistance (measure the rail unloaded and at the amp's full output; the drop over the
   current is the source resistance), `full_load_a` the supply current at full output, `reservoir_f`
   the first filter capacitor, `rectifier` a label.
9. **The panel.** Name the knobs, give the taper of every pot that is not linear (`taper`; a log
   taper is about 2) and its travel (`range`). The `target` of each is fixed (see the table above).
10. **Fill in the rest with defaults, mark what you guessed.** Anything you could not read is
    `"fitted"` or at least annotated with a `note`, so that the next person knows what is a part and
    what is a guess. `metadata.licence` says under which terms the file is shared.
11. **Load it.** `block.loadDescription` (or `tools/match/amp_preset.py`, see below) tells you
    what is wrong and where. Then check the stage gains against the schematic's published figures
    (the small-signal gain of a stage is mu Ra / (Ra + rp) with the cathode bypassed), listen, and
    fit what is left (next section).

## Measuring an amp or a capture and fitting the description

The values that have no component behind them (`"fitted"`: the grid conduction model, the power
stage knee and crossover, the transformer corners, the sag attack, the plate capacitances, the
`calibration` block) are chosen by comparing our amp with the real one. `tools/match` and
`docs/matching.md` do the measuring; nothing of the reference is read or copied, only what comes
out of its output jack.

**What to compare against.** Either the real amp (record its output, line out or a load box with a
direct output, at the knob settings you want to match), or a capture of it (a `.nam` model played
through a NAM block; `PedaleiraRender` renders it with `chain.addToneFromFiles`). A capture is a
device at one set of knob positions: fit at that position, then check the others.

1. **Make the test signal and play it through the reference.** `PedaleiraMatch signals --out
   match/signal` writes `pedaleira-test-signal-v1.wav` (chirp markers, a sweep at -18 and -6 dBFS,
   1 kHz and 100 Hz sines, a staircase from -60 to 0 dBFS, steps, a noise burst).
   `PedaleiraMatch record --device ... --out-ch 1 --in-ch 1 --name amp-gain5` plays it through the
   interface into the amp and records the answer, or render it through the capture. Set the
   interface for the level the amp sees from a guitar, note the knob positions, and keep the
   cabinet or load out of it (or use the same IR on both sides). Several takes help: low gain and
   high gain, bass and treble up and down, presence and depth at the extremes.
2. **Render ours with the same settings.** `tools/match/amp_preset.py` writes a preset that holds one
   `amp.custom` block with your description (embedded, as the app saves it) and the knob settings of the
   take; `PedaleiraRender` renders the test signal through it:

   ```
   python3 tools/match/amp_preset.py --description my-amp.json --out match/amp-gain5.json \
           --set gain=50 --set bass=50 --set middle=50 --set treble=50 --set master=50
   PedaleiraRender --preset match/amp-gain5.json --in match/signal/pedaleira-test-signal-v1.wav \
                   --out match/ours/amp-gain5.wav --tail 4
   PedaleiraMatch analyse --in match/ref/amp-gain5.wav --out match/analysis/ref-amp-gain5.json
   PedaleiraMatch analyse --in match/ours/amp-gain5.wav --out match/analysis/ours-amp-gain5.json
   PedaleiraMatch compare --reference match/analysis/ref-amp-gain5.json \
                          --candidate match/analysis/ours-amp-gain5.json --markdown match/analysis/amp-gain5.md
   ```

   (Knob values are the block's own, 0 to 100 percent; `levelDb`, `bright` and `voicing` also work.
   A command script cannot do this job: block ids are made up as the rig is built, so a script
   cannot name the block it just added. An agent driving the command server can:
   `chain.addBlock` then `block.loadDescription`.)

3. **Read the differences and move the value that causes them.** The first tolerances in
   `docs/matching.md` (1 dB per band, 25 percent in THD) are a sensible target. The table says
   which fitted value each symptom points to:

   | Symptom in `compare` | Move |
   |---|---|
   | A flat level offset (`level gain at 1 kHz`) at every level | `calibration.output_trim` (levels only), or `power_drive_v` (also moves how hard the power stage is driven) |
   | The staircase (static curve) is right at the top and wrong at the bottom, or the reverse; low-level gain wrong | `calibration.input_full_scale_v` (how hard the first grid is driven), stage `bias_current_a` and `grid.*`, `power_amp.open_loop_gain` |
   | THD at -18 dBFS and its change with gain | `grid.cutoff_v`, `grid.contact_potential_v`, `grid.conduction_ohm`, `max_bias_shift_v`; `calibration.input_full_scale_v` |
   | Low-end response, tightness, and how it changes with gain | `coupling_capacitors_f` of the voicing, `calibration.coupling_gain_rise`, `power_amp.transformer.low_corner_hz` |
   | High-end response above 3 kHz | `plate_capacitance_f` of each stage, `input.grid_stopper_ohm` and `miller_f`, `power_amp.transformer.high_corner_hz`, the presence network |
   | Presence or depth have the wrong range | `power_amp.presence.*`, `power_amp.depth.*` (`max_cut`) |
   | Compression, bloom and recovery (the step and burst takes, the high sweep) | the supply: `source_ohm`, `reservoir_f`, `attack_s`, `full_load_a` |
   | Even harmonics, crossover grit at low level | `power_amp.crossover.*`, `knee_mix` |

   The notes recorded by the loader (`AmpDescription::notes`) list every value marked `"fitted"`,
   which is the set free to move; the others are parts, and should only move if the part is wrong.
4. **Keep the result honest.** Change the value, say so in its `note` and set `"provenance": "fitted"`
   (or `"measured"` if you measured the part). Re-run the whole set of takes after every change,
   not only the one that showed the problem: the controls interact.

A loop that tries values (coordinate descent on the score or on the failing metrics) is a short
script around the three commands above: `compare` writes `score`, `allPass` and a `metrics` array
(`group`, `name`, `reference`, `candidate`, `difference`, `allowed`, `pass`) as JSON on stdout.

## The fitted amps

Five descriptions in `resources/amps` were fitted to measurements of reference amps, each starting
from `high-gain.json` (the same four-stage topology) and moving only what the measurements call for:

| File | Name | What it is |
|---|---|---|
| `lead-100.json` | Lead 100 | A lead amp with a soft knee: the clipping builds up over about 7 dB of input. |
| `red-forty.json` | Red Forty | Very high gain into an EL34 power amp with a strong resonance (depth) band. |
| `heavy-hot.json` | Heavy Hot | Red Forty's circuit with a lower `output_trim` (about 2 dB quieter). |
| `modern-three.json` | Modern Three | Clean at low gain (a steep gain law), and a hard knee once driven: the power amp, with heavy feedback, clips first. |
| `cell-four.json` | Cell Four | Lead-like gain with a deep low-end resonance around 50 to 63 Hz. |

What was measured on each reference, with every knob but GAIN at 5: a 1 kHz staircase played 36 dB
down (-96 to -36 dBFS in 3 dB steps), read as the small-signal gain Gs (the lowest steps), the
bend (the input where the gain is 1 dB under Gs) and the ceiling (the highest output); the THD of a
1 kHz sine at -54 dBFS; at GAIN 2, 5, 8 and 10. And the test signal at line level, read as the low
sweep's response, at GAIN 5 and at the extremes of the tone knobs where they were taken. Every
value the fit moved carries `"provenance": "fitted"` and a note that names its target ("fitted to
the Lead 100 measurements: the tone knob responses").

Where the parts come from: the GAIN law is the gain pot's `taper` and `floor_ohm` with the panel's
`taper` and `range`; Gs is `input_full_scale_v`; the ceiling is `output_trim`; the gap between the
bend and the ceiling (how soft the knee is) and the THD come from the bias of the gain stages, the
divider into each grid and `power_drive_v` (which of the preamp and the power amp clips first); the
responses come from the tone stack, the presence and depth networks, the feedback and the
interstage capacitors. `test/src/effects/amp_fitted_tests.cpp` measures each file the same way
(`amp_fit_measure.h`) and checks it against its targets; its `AmpFitHarness.Measure` test prints
the numbers for any file, for a fitting loop:

```
PEDALEIRA_AMP_FIT=my-amp.json build/test/DspTests_artefacts/Release/DspTests --gtest_filter='AmpFitHarness.*'
```

### The clean and crunch amps

Ten more descriptions were fitted the same way to clean and crunch reference amps, each with the
topology of its kind (two to four 12AX7 stages around the gain pot, the tone stack, the power amp)
and the controls of its circuit only:

| File | Name | What it is | Panel |
|---|---|---|---|
| `glass-forty.json` | Glass Forty | A bright 6L6 clean with a lot of headroom; the feedback power amp clips first, with a hard knee. | volume, bass, treble |
| `bell-thirty.json` | Bell Thirty | A chiming EL84 clean, no global feedback; the second stage clips before the voicing network, which keeps the highs when driven. | volume, tone cut |
| `steady-120.json` | Steady 120 | A solid-state clean: stages kept far from their limits, the `solid_state` power amp (linear up to its rail), no sag. Under 0.01 percent THD where the reference reads 0.0. | volume, bass, middle, treble |
| `velvet-fifty.json` | Velvet Fifty | A smooth 6L6 clean with a soft knee and some low-level distortion from its recovery stage. | gain, bass, middle, treble, master |
| `ember-five.json` | Ember Five | A small single-ended EL84 amp, no global feedback (class A without crossover stands for the single-ended stage). | volume, bass, treble |
| `amber-fifty.json` | Amber Fifty | A British-style 50 W EL34 crunch whose GAIN barely moves its small-signal gain. | gain, bass, middle, treble, presence, master |
| `copper-eighteen.json` | Copper Eighteen | A small EL84 crunch, no global feedback. | volume, tone cut |
| `dust-forty.json` | Dust Forty | A tweed-style 6L6 crunch, a little feedback, a tube rectifier. | volume, bass, middle, treble, presence |
| `bronze-twenty.json` | Bronze Twenty | A chiming EL84 crunch, no global feedback. | volume, bass, treble, tone cut |
| `silk-hundred.json` | Silk Hundred | A smooth overdrive channel: four 12AX7 stages into a 6L6 power amp with feedback. | drive, bass, middle, treble, presence, master |

These references were played 24 dB (clean) or 36 dB (crunch) under their nominal level: the
staircase from -84 or -96 dBFS, the THD at the staircase's -18 dBFS step (-42 or -54 dBFS), and the
test signal itself, read as the low sweep's response and the 1 kHz gain of both sweeps with every
knob at 5, and as the change of the response when each tone knob goes from 0 to 10 at GAIN 2.
`test/src/effects/amp_fitted_clean_crunch_tests.cpp` checks each file against them; the harness
takes the same settings:

```
PEDALEIRA_AMP_FIT=resources/amps/glass-forty.json PEDALEIRA_AMP_FIT_OFFSET=24 \
PEDALEIRA_AMP_FIT_TONES=bass,treble \
  build/test/DspTests_artefacts/Release/DspTests --gtest_filter='AmpFitHarness.*'
```

Two of them take another knob for their GAIN sweep (`PEDALEIRA_AMP_FIT_KNOBS`): the reference
Velvet Fifty's sweep is 6 dB under its level with every knob at 5 (its master at 3.5 fits), and the
reference Silk Hundred's about 5 dB over it (its master at 6.7). Where the fit falls short is in the
test file: the highs of the crunch amps' responses (the clipping comes after the tone stack, so the
sweep compresses the treble the reference keeps), a few tone knob bands where the treble, middle
and bass network cannot take the reference's shape, and the THD of Ember Five at VOLUME 8 and 10
(18 and 33 percent within a few dB of the bend, more than the framework's curves make).

## Licences

- **The code** of the framework (the components, the loader, the amp kinds, the tools) is under
  the licence of this repository, MIT (`LICENSE`).
- **chowdsp_wdf** (Jatin Chowdhury, https://github.com/Chowdhury-DSP/chowdsp_wdf) is BSD-3-Clause.
  It is fetched at build time at the pinned release tag v1.0.0 through CPM (`CMakeLists.txt`,
  into the gitignored `libs/chowdsp_wdf`), is header only, and is linked into the targets that
  compile the effects. Nothing of it is copied into this repository. Its copyright notice and
  licence text stay with the library; a binary distribution that includes it carries them (the
  library's `LICENSE`).
- **nlohmann json** (MIT) reads the descriptions; it already ships with the NAM core.
- **A description is data, and it belongs to whoever writes it.** `metadata.licence` states the
  terms of that file: a description you wrote from a schematic of your own amp is yours to share
  under any licence you pick (the ones shipped here are MIT). A schematic can be someone else's
  copyrighted drawing, and a description of a particular product can name a trademark: you are
  writing facts about a circuit (values of parts), not copying a design file, but check the
  terms of your source and do not ship another maker's models, captures or data as if they were
  yours. Pedaleira's own rules apply to what is added to this repository: no code, models or data
  from another product, no decompiling or decrypting, and an identity of its own.

## What is not modelled

- Stages that are not common-cathode voltage amplifiers (cathode followers, long-tailed pairs,
  a recovery stage and phase inverter): the level they add is absorbed in `calibration`.
- The effects loop (a placeholder: the signal does not pass through one), speaker and cabinet
  (use an IR block), microphonics, heater hum, power supply ripple and the screen grid.
- A tone stack that is not the treble, middle and bass (TMB) circuit, and the source impedance
  that drives the tone stack.
- A single-ended output stage: its asymmetric, partly expanding curve (class A push-pull, without
  crossover, is the nearest the power amp has).
- Grid current loading the coupling network exactly: the grid-conduction bias shift is a fitted
  behavioural element in both stage models.
- Anti-aliasing of the wave digital stage beyond the 4x oversampling.

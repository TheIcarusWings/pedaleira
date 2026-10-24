# sc sweep 1: time behaviour, one change at a time

Goal: learn which time-behaviour parameters move MR-STFT sc towards the QC takes for Red Forty,
Heavy Hot, Cell Four and Modern Three (GAIN 9 chugs is the target there), without hurting tonal.
QC stays off. No script to run.

## Build

Current clone at 85dc886 (its amp code is the same as main 66727c8). No rebuild needed: only
description files change.

## Candidates

`candidates.json` lists 24 candidates, c00 to c23, each one change from the main description
(c00 is main itself, as a check that it reproduces the 85dc886 renders). Each amp folder holds
one file per candidate, and it replaces that amp's description:

| Folder | Amp | Tag |
|---|---|---|
| `red-forty/` | Red Forty | ev101red-el34 |
| `heavy-hot/` | Heavy Hot | gojira-hot |
| `cell-four/` | Cell Four | dcell-h4-ch3 |
| `modern-three/` | Modern Three | caduo-modern |

Render each file with amp_preset.py --description, as in the round 2 renders.

## What to render and score

- Settings: all5 (GAIN 5), gain8 (GAIN 8), g7 (GAIN 7) and g9 (GAIN 9), other knobs at 5, every
  take each setting has (14 takes per amp).
- IR condition: ir7806 only, on both sides, level matched, as in round 2.
- BEFORE: the 85dc886 renders you already have.
- DspTests: no.

## What to send back

One message per amp if it helps, each starting with the folder name
`2026-10-24-sc-sweep1` and the amp. In it:

1. A check line: c00 against the 85dc886 render (the largest render-against-render esr over
   the takes). It should be far below -60 dB.
2. Per candidate, the means over the takes of the round 2 columns
   [sc, lmm, esr, tonal, level, crest, attack]: one row for all 14 takes, one for lead and riff
   only (all5, gain8, g7 lead, g9 lead: 6 takes), and one for chugs only (4 takes).
3. Per candidate and take, only [sc, tonal] as a compact JSON, keyed `setting/take`.
4. The zones of each candidate over the 14 takes (sc close / gap / far, tonal close / gap / far)
   with the round 1 bounds (sc 0.352 and 0.411, tonal 3.53 and 4.22).

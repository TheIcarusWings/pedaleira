# sc sweep 2: combinations around the sweep 1 winners

Same build, conditions and scoring as `2026-10-24-sc-sweep1` (current clone at 85dc886, no
rebuild, IR 7806 on both sides, level matched, settings all5, gain8, g7 and g9, 14 takes per amp,
no DspTests, no script). QC stays off.

## Candidates

`candidates.json` lists the candidates per amp; their ids start with `d`, and d00 is main in
every folder. The candidate lists differ per amp:

| Folder | Amp | Tag | Candidates |
|---|---|---|---|
| `red-forty/` | Red Forty | ev101red-el34 | d00 to d21 |
| `heavy-hot/` | Heavy Hot | gojira-hot | d00 to d21 (the same changes as Red Forty) |
| `modern-three/` | Modern Three | caduo-modern | d00 to d08 |
| `cell-four/` | Cell Four | dcell-h4-ch3 | d00 to d14 |

## What to send back

The same four parts as sweep 1, per amp, each message starting with `2026-10-24-sc-sweep2` and
the amp: the c00 check (here d00 against the 85dc886 render), the means per candidate (all14,
leadriff, chugs), the per take [sc, tonal] JSON and the zones. The digest was useful, so please
keep it, with g9/chugs in it as before.

# sc sweep 3: refinement, and a Cell Four diagnostic

Same build, conditions and scoring as sweeps 1 and 2 (current clone at 85dc886, no rebuild,
IR 7806 on both sides, level matched, settings all5, gain8, g7 and g9, 14 takes per amp, no
DspTests, no script). QC stays off. Candidate ids start with `e`; e00 is main in every folder.

| Folder | Amp | Tag | Candidates |
|---|---|---|---|
| `red-forty/` | Red Forty | ev101red-el34 | e00 to e19 |
| `heavy-hot/` | Heavy Hot | gojira-hot | e00 to e19 (the same changes as Red Forty) |
| `modern-three/` | Modern Three | caduo-modern | e00 to e06 |
| `cell-four/` | Cell Four | dcell-h4-ch3 | e00 and e01 (the polarity fix) |

## What to send back

For Red Forty, Heavy Hot and Modern Three: the same four parts as before (check, means, per
take [sc, tonal] JSON, zones) and the digest. Please add to the means one row per setting
(all5, gain8, g7, g9: the mean sc and the mean tonal of that setting's takes) for every
candidate.

## Cell Four diagnostic

No parameter of sweeps 1 and 2 moved Cell Four's sc (0.417 to 0.47), so this asks where the
difference is rather than trying more values. For e01 against the QC takes, on all5/chugs,
all5/riff, gain8/chugs and gain8/lead (IR 7806, polarity and level matched as in the scores):

1. sc (FFT 1024) split into four frequency ranges: below 250 Hz, 250 Hz to 1 kHz, 1 to 4 kHz,
   above 4 kHz. For each range, the sc computed on that range's bins only.
2. The same sc split in time: frames within 30 ms after a note onset against all other frames
   (onsets from the QC take's envelope, your own simple detector is fine; say which).
3. The level in dB, ours minus QC, of those four ranges, separately for the onset frames and
   the other frames.
4. Over the whole take, the short-term envelope (RMS over 10 ms frames, in dB) of ours against
   the QC take: their correlation, and the mean and the 90th percentile of ours minus QC.

Send it as one message starting with `2026-10-24-sc-sweep3 cell-four diagnostic`.

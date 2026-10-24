# sc sweep 3b: the bias-shift ceiling alone

Run after `2026-10-24-sc-sweep3`. Same build, conditions and scoring (current clone at 85dc886,
no rebuild, IR 7806 on both sides, level matched, settings all5, gain8, g7 and g9, 14 takes per
amp, no DspTests, no script). QC stays off.

Why: the capacitor changes that lowered sc in sweeps 2 and 3 also move the small-signal gain
and response away from the bench measurements the amps are fitted to. The bias-shift ceiling
does not touch the small-signal behaviour, so this measures how much of the sc gain it gives
on its own.

Candidates f00 to f04 per folder (f00 is main): `grid.max_bias_shift_v` 10 (main), 1, 0.7, 0.5
and 0.3 on every stage, for Red Forty (ev101red-el34), Heavy Hot (gojira-hot), Modern Three
(caduo-modern) and Cell Four (dcell-h4-ch3).

Send back the means (all14, leadriff, chugs, and one row per setting with mean sc and mean
tonal), the zones and the digest. The per take JSON is not needed for this one.

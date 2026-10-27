# Cell Four sweep 4: less preamp drive, and where tonal moves

Build: current clone; main is now f2a82c9 (PR 45 removed Cell Four's invert_output, PR 47
changed only Red Forty and Heavy Hot), so please rebuild the render tool at f2a82c9 first. Same
conditions and scoring as the sweeps of 2026-10-24 (IR 7806 on both sides, level matched,
settings all5, gain8, g7 and g9, 14 takes). QC stays off. No script, no DspTests.

Candidates h00 to h04 in `cell-four/` (h00 is main): `calibration.input_full_scale_v` times 1,
0.85, 0.7, 0.5 and 0.35, with `output_trim` divided by the same factor so the small-signal gain
stays. They replace the Cell Four description (dcell-h4-ch3).

Send back, per candidate:
1. the means (all14, leadriff, chugs) and one row per setting with mean sc and mean tonal;
2. the zones and the digest;
3. new: the tonal difference per band, ours minus QC in dB, re 1 kHz, for the 24 third-octave
   bands the tonal score uses, as the mean over the 14 takes and also the mean over the chugs
   and riff takes only. This says which EQ the refit needs.
4. the diagnostic envelope table of sweep 3 (correlation; mean ours minus QC in the quietest
   quarter, the middle half and the loudest quarter of the frames) for h00 and h03 on all5/chugs,
   all5/riff and gain8/chugs.

# Inpaint opposed: too dark where two channels clip on warm lights

Status: done (2026-10-25): fix found and scored; Kofa's decision at the end. Before: wip. Kind: experiment. Kofa's choice (c) at the end of issue 29
([opposed-brightness.md](opposed-brightness.md)).

## Goal

Make inpaint opposed's partly clipped pixels around bright warm lights as bright as the scene,
without a hue ratio fill (patent claim 20, see [merge-readiness.md](merge-readiness.md)), so that a
brightness fill of the fully clipped core (issue 29) starts from a rim at the right level.

## Context

- Opposed (`src/iop/hlreconstruct/opposed.c`, `_process_opposed()`): a clipped photosite of channel
  c becomes `max(value, ref_c + offset_c)`; `ref_c` is the cube of the mean of the cube roots of the
  two other channels' 3x3 means (`_calc_refavg()`, `segbased.c:196`), `offset_c` one global linear
  offset per channel, measured on unclipped photosites near clipped ones.
- Measured on the real pairs (`/tmp/hl-rules-study/tasks/2026-10-25-ring/ringdef.py`,
  `ringdef-opposed.txt`; log2(truth / opposed), Rec2020 luminance, partly clipped pixels with a
  truth): pooled over all pairs the partly clipped pixels are right (median -0.03 to 0.01 EV), but
  night scenes with two clipped channels are 0.16-0.18 EV too dark, colored ones about 0.1 EV; the
  spread within 4 px of fully clipped pixels is large (90th percentile 0.4-0.6 EV). City and phone
  scenes are too bright instead (phone: the GainMap, learnings.md).
- Per pair (`ringpairs.py`, `ringpairs.txt`; within 16 px of fully clipped pixels): the worst are
  two clipped channels, red and green, on warm lights: g294 corridor lamp 0.54 EV, g065 sunset 0.44,
  g298 atrium lights 0.32, g403 0.20, g300 0.14, t53 and g486 0.12.
- Why (reading of the code): with red and green clipped and only blue measured, each clipped
  channel's reference averages the other clipped channel at its clip value with the low blue, so
  `ref + offset` stays below the clip level and both channels stay at clip; the global offset is
  measured mostly elsewhere in the image and does not grow with the light's brightness.
- Patent: claim 20 covers clipped channels as "values based on the non-saturated channels"
  multiplied by "values based on the estimated hues", ratios of saturated to non-saturated
  channels. A ratio carried inward from the rim is excluded; opposed's additive form is not a ratio.

## Steps

1. Offline harness with exact truth (synthetic clipping of the darker frames in camera space, as
   issue 25's `predict.py`), brightness bias and error per class (one / two / three clipped
   channels): opposed, offset in cube-root space, iterated references, per-region offsets, their
   combinations; a log-space offset (a ratio) as a reference only. Luna, `ring.py`.
2. If a non-ratio variant fixes the two-channel class without hurting skies and city scenes: render
   it (new darktable experiment copy from this branch, switch in `opposed.c`) and score on the 103
   pairs; then redo issue 29's fill on top.

## Done when

The offline harness shows no non-ratio variant that helps (then stop and report), or the chosen
variant is scored on the real pairs.

## Offline harness (2026-10-25, Luna; `/tmp/hl-rules-study/tasks/2026-10-25-ring/`)

`ring.py` (from issue 25's `predict.py`): the darker frame of each pair at the brighter frame's
exposure, 2x2 quads in camera space, clipped synthetically at 0.98 of each channel's threshold, so
every clipped quad has an exact truth; opposed's quad approximation (no 3x3 window: each quad is
independent). Checked against `predict.py`'s OPP on three pairs: identical. 103 pairs, 5 minutes.
Bias = median log2(estimate / truth) of the clipped values (negative = too dark), abs = mean
|log2|, win / tie / loss against opposed on abs (tie 0.02 EV) and angle (tie 0.1 degree).

| predictor (two clipped channels, 96 pairs) | bias | abs | angle | abs W/T/L | angle W/T/L |
|---|---|---|---|---|---|
| opposed | -0.111 | 0.172 | 3.68 | - | - |
| offset in cube-root space | -0.115 | 0.169 | 3.58 | 5/89/2 | 37/47/12 |
| iterated references, 1 more pass | -0.073 | 0.138 | 3.40 | 45/36/15 | 61/14/21 |
| iterated, 10 passes (converged) | -0.040 | 0.162 | 3.24 | 47/30/19 | 63/13/20 |
| iterated, cube-root offset, converged | -0.004 | 0.163 | 3.51 | 44/26/26 | 59/6/31 |
| per-region linear offsets, iterated 10 | -0.026 | 0.134 | 2.86 | 57/27/12 | 74/9/13 |
| per-region cube-root offsets, iterated 10 | 0.024 | 0.151 | 3.21 | 46/22/28 | 63/6/27 |
| log-space offset (a ratio, reference only) | -0.109 | 0.170 | 3.61 | 16/72/8 | 43/32/21 |

- "Iterated references": a clipped channel's reference uses the other clipped channel's estimate
  of the previous pass instead of its clip value. For two clipped channels it converges (the
  coupling has gain 0.5); for three it does not (each pass adds the offsets again), so fully
  clipped quads keep the first estimate (`ring2.py`). Converged within 10 passes.
- With the offset in cube-root space, the converged two-channel estimate has a closed form (Gemini,
  `answer-gemini.md`): `R = (cbrt(B) + d_rb)^3`, an additive offset in cube-root space on the
  measured channel. Gemini's view that additive offsets do not read on claim 20 is its opinion.
- On the warm-light pairs (two clipped channels, bias): g294 -0.46 -> -0.26 (iterated) / -0.12
  (cube-root, iterated); g065 -0.43 -> -0.19 / -0.02; g298 -0.31 -> -0.11 / -0.10; overshoot on
  t60 (+0.21) and g486 (+0.30) with the converged variants.
- Per scene kind, the per-region iteration wins everywhere (city 18 / 8 on abs, night 11 / 0, sky 6
  / 2); the global iteration loses on city in abs (0.112 -> 0.169 median).

## Real renders (2026-10-25)

Experiment copy `/tmp/hl-rules-study/dt-src` (this branch, upstream e1e2e26baa, without `.git`),
build `/tmp/hl-rules-study/dt-build` (Release, OpenCL, Lua and AI off). TEMPDEBUG switches in
`src/iop/hlreconstruct/opposed.c`, `_process_opposed()`, CPU only: `DT_OPP_ITER=n` (n more passes
for clipped photosites with an unclipped photosite of another channel in the 3x3 window; the
others keep the first estimate), `DT_OPP_CBRT=1` (offset in cube-root space), `DT_OPP_ITER_RG=1`
(iterate only red and green photosites whose window has the other of the two clipped and blue
measured). With the switches unset the code path is unchanged; the rebuilt opposed (`opp-b`)
matches the existing `opposed.pfm` (max relative difference 0.002 on g294). Methods in `ev4.py`.

SSIMULACRA 2 at -2 / -3 EV against inpaint opposed (`pair_align-ring.json`, `table-*.txt`):

| variant | all (W/L/T/M) | city | night | sky | phone |
|---|---|---|---|---|---|
| `opp-it1`: 1 more pass | 18 / 19 / 66 / 0 | 6 / 14 | 3 / 0 | 3 / 2 | 6 / 0 |
| `opp-it10`: 10 passes | 8 / 29 / 64 / 2 | 1 / 20 | 4 / 0 | 2 / 4 | 0 / 2 |
| `opp-cit10`: cube-root offset, 10 passes | 22 / 37 / 44 / 0 | 3 / 19 | 5 / 2 | 1 / 6 | 9 / 0 |
| `opp-c`: cube-root offset only | 21 / 7 / 75 / 0 | 9 / 0 | 1 / 2 | 1 / 0 | 9 / 0 |

Brightness of partly clipped pixels with two clipped channels on the real renders (`ringdef.py`,
log2(truth / method), median; within 4 px of fully clipped / 5-16 px):

| | night | colored | city | sky |
|---|---|---|---|---|
| opposed | 0.18 / 0.16 | 0.09 / 0.12 | -0.12 / -0.14 | 0.05 / 0.02 |
| `opp-it1` | 0.16 / 0.16 | 0.10 / 0.12 | -0.09 / -0.12 | 0.07 / 0.05 |
| `opp-it10` | 0.11 / 0.15 | 0.08 / 0.10 | -0.25 / -0.29 | -0.03 / -0.05 |
| `opp-cit10` | 0.03 / 0.08 | 0.02 / 0.03 | -0.24 / -0.28 | -0.06 / -0.10 |
| `opp-c` | 0.19 / 0.18 | 0.10 / 0.13 | -0.09 / -0.11 | 0.05 / 0.02 |

- City skies with two clipped channels (green and blue) are already brighter than the truth with
  opposed; the iteration makes them brighter still, hence the city losses. The harness did not
  show this (its city abs got worse, but its win counts did not).
- Visual check (`sheet-ring.png`, hue-preserving display, -2 EV): the cube-root offset turns skies
  pink to magenta (g065 strongly, t20, and the g294 lamp pinkish): out by Kofa's rule, whatever the
  score. The iteration gives lamps the bright white rim the truth has (g298, t60), but adds lilac or
  pink to skies (t45, the g213 cloud edges, g065).

### Iteration only for warm lights (`DT_OPP_ITER_RG=1`)

Red and green photosites are iterated only where the window has the other of the two clipped and
blue measured; everything else keeps opposed's estimate. Offsets stay linear (opposed's), so the
fully clipped fill keeps opposed's color.

| against inpaint opposed (`pair_align-ring2.json`, `table2-*.txt`) | all (W/L/T/M) | night | indoor | other kinds |
|---|---|---|---|---|
| `opp-rg10`: 10 passes | 4 / 1 / 98 / 0 | 4 / 0 (g294, g298, g300, t53) | 0 / 1 (t60 0034) | all ties |
| `opp-rg2`: 2 passes | 5 / 1 / 97 / 0 | 4 / 0 | 1 / 1 | all ties |
| `opp-it1` (repeat of the first run) | 18 / 19 / 66 / 0 | 3 / 0 | 0 / 1 | city 6 / 14 |

- Ring brightness (two clipped channels, night, within 4 px of the core): 0.18 -> 0.11 EV too
  dark, 90th percentile 0.74 -> 0.49; colored 0.09 -> 0.09, near / mid; city and sky unchanged.
- Visual (`sheet-rg.png`, -2 / -3 EV): the lamps get the bright white rim of the truth (g298, t60,
  t53); no new tint anywhere; g065's sky changes only at its edge.
- Cost: whole darktable-cli renders took 15.2 s (10 passes) against 15.0 s (2 passes) on average
  (4 parallel workers, 3 threads each; not a benchmark of the module). Each pass is one sweep over
  the clipped photosites of a 3x3 window.

### With issue 29's fill on top (`rg-hdomelin`: distance dome on `opp-rg10`)

| `pair_align-ring3.json`, `table3-*.txt` | all (W/L/T/M) | night | sky | city | colored |
|---|---|---|---|---|---|
| `opp-rg10` vs opposed | 4 / 1 / 98 / 0 | 4 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| distance dome (`opp-hdomelin`) vs opposed | 22 / 7 / 74 / 0 | 0 / 1 | 9 / 0 | 2 / 3 | 5 / 3 |
| dome on the fixed rim (`rg-hdomelin`) vs opposed | 27 / 7 / 69 / 0 | 5 / 1 | 9 / 0 | 2 / 3 | 5 / 3 |
| `rg-hdomelin` vs `opp-hdomelin` | 7 / 1 / 95 / 0 | 5 / 0 | 0 / 1 (g315) | 0 / 0 | 1 / 0 |

Fully clipped deficit (median log2(truth / method), pixels with a truth): t60 0.44 -> -0.06, t53
0.37 -> -0.05, g298 0.63 -> 0.29, g294 0.79 -> 0.57, g065 0.64 -> -0.09; g300 overshoots more
(-0.25 -> -0.46). Over 69 pairs: median 0.20 -> 0.08 EV, quartiles 0.01 / 0.37 -> -0.15 / 0.20.
With the rim fixed, the fill now reaches the lamps that motivated issue 29.

Crops (`sheet-full-rg.png`, `sheet-lamps-rg.png`; -2 / -4 EV): brighter, whiter lamp rims and
brighter cores at -4 EV (t60, g298, n041). One visible error remains on lamps: opposed fills their
fully clipped cores light blue where the truth is white (t60, g298, t53); neither change touches
that hue.

## Conclusions (2026-10-25)

1. The dark ring is the mutual clamping of two clipped channels in opposed's reference. Using the
   other clipped channel's estimate instead of its clip value (iterating) fixes it where red and
   green clip and blue is measured. This is opposed's own model solved consistently: no ratio, no
   new offset. Whether that keeps clear of claim 20 is a legal question, but it introduces nothing
   ratio-like that opposed does not already have.
2. Iterating everywhere hurts city skies (green and blue clipped, already too bright), and the
   cube-root offset turns skies pink: both are out. The harness (independent quads, brightness and
   angle only) did not predict either; only renders and crops did.
3. Restricted to warm lights it is safe: 4 / 1 against opposed, every other pair a tie, no tint.
   Its gain is small in pairs, but it makes issue 29's fill work on lamps: together 27 / 7.
4. Remaining on lamps: the light blue hue of opposed's fully clipped cores (a separate issue).

## Decision for Kofa

- Look at `sheet-rg.png` (rim fix alone), `sheet-full-rg.png` and `sheet-lamps-rg.png` (with the
  dome) in `/tmp/hl-rules-study/tasks/2026-10-25-ring/`.
- Options: (a) take the rim fix (`DT_OPP_ITER_RG`, 10 or 2 passes) as a candidate for opposed.c
  proper (OpenCL path and a GUI-free default would be needed; I would also check X-Trans, issue 20,
  since the rule is written for colors, not for a Bayer layout); (b) the rim fix plus issue 29's
  dome, ported to C (the dome is the larger piece: Laplace solve, distance transform); (c) look at
  the blue lamp cores first. My recommendation: (b) as one experiment in the copy, with (c) noted;
  the dome is what moves skies and lamps, the rim fix is what lets it reach lamps.

Files: `/tmp/hl-rules-study/tasks/2026-10-25-ring/` (`ring.py`, `ring2.py`, `ring3.py` and their
`.json` / `.md`; `ringdef.py`, `ringpairs.py`, `ringdef-*.txt`; `run_render.sh`, `run_score.sh`;
tables `table*-*.txt`; sheets `sheet-ring.png`, `sheet-rg.png`, `sheet-full-rg.png`,
`sheet-lamps-rg.png`; Luna's and Gemini's answers). Experiment copy and build kept:
`/tmp/hl-rules-study/dt-src`, `/tmp/hl-rules-study/dt-build` (the opposed.c switches; `ev4.py`
methods `opp-b`, `opp-it1`, `opp-it10`, `opp-cit10`, `opp-c`, `opp-rg10`, `opp-rg2`). Copies in
`tools/opposed-ring/` (scripts and the patched `opposed.c`). Renders kept in the workdirs:
`opp-rg10`, `rg-hdomelin` (and issue 29's `opp-hdomelin`).

Removed on 2026-10-25 (regenerable with the build and `ev4.py`): renders `opp-b` (g294 only),
`opp-it1`, `opp-it10`, `opp-cit10`, `opp-c`, `opp-rg2`; preview JPEGs.

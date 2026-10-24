# Blend inpaint opposed with gradient domain's hue; opposed's offset as a smooth field

Status: done (2026-10-24): negative by Kofa's look; the blend carries gradient domain's magenta into skies. Score favored it (30 / 12 at 0 / -1 EV, 52 / 4 at -2 / -3 EV). Kind: experiment (Claude's idea, during Kofa's absence).

## Goal

Find a fill for partly clipped pixels that beats inpaint opposed on skies and city scenes and keeps
gradient domain's gains on lights, using the cheap offline harness of issue 25 to screen first.

## Ideas

- **OFS-LIN**: inpaint opposed's reference (mean of the other channels in cube-root space) plus an
  offset that varies in space: the linear offset `value - reference` of bright border pixels
  (unclipped in the channel, between 0.2 and 0.98 of its clip level, within 3 quads of the clipped
  area) is interpolated over the clipped area by a Laplace solve. Dark border pixels carry no value,
  so occluders do not anchor the field. This is the smooth version of issue 9's per-region offsets.
  **OFS-CBRT**: the same with the offset in cube-root space.
- **OPPxHUE t**: per channel, the geometric blend `OPP^(1-t) * HUE^t` of inpaint opposed's estimate
  and gradient domain's hue estimate (issue 25's HUE: Laplace interpolation of the border
  chromaticity, then ratios).

## Offline result (camera space, synthetic clipping, 103 pairs)

Script `/tmp/hl-rules-study/tasks/2026-10-24-offset-field/` (`offsetfield.py`, `variants.py`,
`summarize.py`; results `offsetfield.json`, `variants.json`). Same harness, pairs and metric as
issue 25: per-pair mean RGB angle error on partly clipped quads, wins / ties (0.1 degree) / losses.

| predictor | vs OPP | vs HUE | one clipped vs OPP | two clipped vs OPP | median of the mean angle |
|---|---|---|---|---|---|
| OPP | - | 50 / 11 / 42 | - | - | 1.50 deg |
| HUE | 42 / 11 / 50 | - | 24 / 19 / 60 | 45 / 6 / 45 | 1.69 deg |
| OFS-LIN (ring 3; 1 and 8 alike) | 50 / 33 / 20 | 56 / 11 / 36 | 40 / 38 / 25 | 56 / 25 / 15 | 1.50 deg |
| OFS-CBRT | 51 / 19 / 33 | 50 / 20 / 33 | 34 / 22 / 47 | 68 / 11 / 17 | 1.54 deg |
| **OPPxHUE 0.25** | **67 / 30 / 6** | 62 / 7 / 34 | 34 / 60 / 9 | 81 / 5 / 10 | 1.33 deg |
| OPPxHUE 0.5 | 56 / 23 / 24 | 72 / 11 / 20 | 35 / 41 / 27 | 69 / 6 / 21 | 1.21 deg |
| OPPxHUE 0.75 | 55 / 10 / 38 | 78 / 15 / 10 | 33 / 25 / 45 | 56 / 6 / 34 | 1.44 deg |
| OFSxHUE 0.25 | 61 / 22 / 20 | 61 / 14 / 28 | 37 / 35 / 31 | 76 / 7 / 13 | 1.33 deg |
| geometric mean of OPP, OFS-LIN, HUE | 60 / 24 / 19 | 67 / 9 / 27 | 37 / 41 / 25 | 73 / 9 / 14 | 1.30 deg |

By scene against OPP (win / tie / loss):

| predictor | city | colored | indoor | night | phone | sky | specular |
|---|---|---|---|---|---|---|---|
| HUE | 10/3/28 | 6/1/7 | 5/1/2 | 12/1/0 | 4/2/3 | 2/2/8 | 3/1/2 |
| OFS-LIN | 8/16/17 | 7/5/2 | 7/1/0 | 12/1/0 | 7/2/0 | 6/5/1 | 3/3/0 |
| OPPxHUE 0.25 | 16/19/6 | 10/4/0 | 7/1/0 | 13/0/0 | 8/1/0 | 8/4/0 | 5/1/0 |
| OPPxHUE 0.5 | 11/11/19 | 7/5/2 | 6/1/1 | 13/0/0 | 8/1/0 | 6/4/2 | 5/1/0 |

- OPPxHUE 0.25 wins in every scene kind, skies and night scenes alike, with 6 losses in all. The
  errors of the two predictors are partly independent, so their blend is better than either.
- OFS-LIN wins on lights and phone pairs like HUE but loses 17 city pairs; it is not better than the
  blend.
- An adaptive weight, `t0 * exp(-|log2(HUE/OPP)| / s)` per pixel (less hue where the two disagree),
  does not beat the plain 0.25 blend: best ADA(0.25, 1) 66 / 32 / 5 against 67 / 30 / 6
  (`adaptive.py`, `adaptive-summary.txt`; s = 0.25 gives 50 / 51 / 2, t0 = 0.5 more losses).
- Limits: the harness scores the chromaticity of partly clipped quads only. Fully clipped pixels,
  gradient domain's detail (gradient) part and display-referred visibility are not in it.

## Render-level check

`/tmp/hl-rules-study/tasks/2026-10-24-blend/blend.py` blends the existing renders per channel in
linear Rec2020 (`opposed.pfm`, `gd-r-mg4.pfm`, the multigrid gradient domain at reduced resolution):
`blend25` (t = 0.25), `blend50`. This is not the same as a camera-space blend, and fully clipped
pixels are blended too. Runner `runs/run_blend.sh`, scores `pair_align-blend.json` (SSIMULACRA 2,
PA_HALF=1), tables `runs/blend-*-vs-*.txt` (pair_table.py, tie |d| < 2).

| comparison (103 pairs) | win | loss | tie | mixed |
|---|---|---|---|---|
| gradient domain against inpaint opposed (same run) | 26 | 49 | 27 | 1 |
| blend25 against inpaint opposed | 30 | 13 | 60 | 0 |
| blend50 against inpaint opposed | 32 | 24 | 47 | 0 |
| blend25 against gradient domain | 52 | 22 | 28 | 1 |
| blend50 against blend25 | 20 | 24 | 59 | 0 |

blend25 against inpaint opposed, by type: city 3/5/33, colored 4/2/8, indoor 3/1/4, night 7/0/6,
phone 9/0/0, sky 2/5/5, specular 2/0/4 (win / loss / tie). For comparison: local offsets (issue 9)
16 / 6, the per-image clipped-share rule (issue 26) 14 / 7 to 19 / 10 in leave-one-scene-out.

- Nearly all differences are at -1 EV, where the highlights are visible.
- Largest losses: g315 lake glare -21.3, g319 sun through trees -10.3, g353 yellow tree -8.7,
  g213 -6.0, g057 -5.6, t27 -5.2, t45 -5.0. On g315 and g319 gradient domain alone fails by -77.6 and
  -56.4 (fully clipped glare); the blend takes a quarter of that failure.
- Largest wins: the phone pairs (+10 to +33), g065 sunset +18.0, g294 +11.5, g117 +11.1, g281 +10.9,
  t60 +10.0, g298 +10.1.

Partly clipped pixels only (weight `t * (1 - F_min)`, F = the clipped frame's per-channel clipped
share from `masks.npz`, so fully clipped pixels stay opposed's; `runs/run_blendp.sh`,
`pair_align-blendp.json`) is worse:

| comparison (103 pairs) | win | loss | tie |
|---|---|---|---|
| blend25p against inpaint opposed | 24 | 16 | 63 |
| blend50p against inpaint opposed | 31 | 27 | 45 |
| blend25p against blend25 | 1 | 10 | 92 |

The fully clipped pixels' share of the blend helps (g281, g313 turn from wins into a tie and a loss).
g315 stays at -22: its failure is in the partly clipped pixels. The same opposed render scores up to
about 1 apart between the two runs (g313 at -1 EV: 65.1 and 66.2): the alignment is redone each run.

### Camera-space prototype: blend the hue inside gradient domain (negative)

`DT_GD_HUE_BLEND=t` in `/tmp/hl-rules-study/dt-src` (`gradient.c`, after the opposed hue block):
partly clipped pixels get the per-channel geometric blend of gradient domain's hue estimate (`rho`,
after the model hue) and `_gd_opposed_hue()`'s estimate, `gd^t * opp^(1-t)`; the Poisson solves then
run as usual. Methods `gd-r-hb25`, `gd-r-hb50` (`scripts/ev4.py`, reduced resolution, multigrid),
runner `runs/run_hueblend.sh`, scores `pair_align-hueblend.json`, tables `runs/hueblend-*.txt`.

| comparison (103 pairs) | win | loss | tie | mixed |
|---|---|---|---|---|
| gd-r-hb25 against inpaint opposed | 27 | 29 | 46 | 1 |
| gd-r-hb50 against inpaint opposed | 30 | 37 | 36 | 0 |
| gd-r-hb25 against gradient domain | 39 | 20 | 43 | 1 |

Better than gradient domain, far worse than blending the two outputs (30 / 13): the guided solves
spread the blended hue into the fully clipped pixels and keep gradient domain's failures there. The
useful form is the output blend: run both methods, blend their results.

### darktable prototype of the output blend

`DT_HL_BLEND=t` in `/tmp/hl-rules-study/dt-src/src/iop/highlights.c` (raw path, gradient domain
mode): after `_process_gradient()`, `_process_opposed()` runs into a second buffer with opposed's
own clipping threshold, and each photosite becomes `opp^(1-t) * gd^t` (linear mix where either is
not positive). Method `gd-r-ob25` (`scripts/ev4.py`), runner `runs/run_outblend.sh`, scores
`pair_align-outblend.json`, tables `runs/outblend-*.txt`. dt-build's highlights plugin rebuilt with
it (Sol's round-6 builds read `highlights.c` from dt-src too; the block is off unless the variable
is set).

| comparison (103 pairs) | win | loss | tie | mixed |
|---|---|---|---|---|
| gd-r-ob25 against inpaint opposed | 30 | 12 | 61 | 0 |
| gd-r-ob25 against the render-space blend25 | 0 | 0 | 103 | 0 |
| gd-r-ob25 against gradient domain | 52 | 23 | 27 | 1 |

The photosite blend in camera space gives the render-space result: on t60 the two differ by 0.002
on average over the changed pixels, against 0.23 between the two methods. Wall time per render at
3 threads up to 29 s (render_extra.py, including darktable's start).

### Blend weight t (render space)

`runs/run_blendt.sh`, `pair_align-blendt.json`, tables `runs/blendt-*.txt`; against inpaint opposed
(blend50 from the first run):

| t | win | loss | tie |
|---|---|---|---|
| 0.15 | 23 | 5 | 75 |
| 0.25 | 30 | 13 | 60 |
| 0.35 | 35 | 16 | 52 |
| 0.5 | 32 | 24 | 47 |

A smooth trade: more of gradient domain wins more on lights, night and phone pairs and loses more
on skies. t works like a strength slider; 0.25-0.35 has the best balance on this set.

### Blend with the local-offsets opposed (issue 9)

`lblend25` = `opp-local^0.75 * gd^0.25` (`BL_A=opp-local`), `runs/run_blendl.sh`,
`pair_align-blendl.json`, tables `runs/blendl-*.txt`:

| comparison (103 pairs) | win | loss | tie |
|---|---|---|---|
| opp-local against inpaint opposed (same run) | 16 | 6 | 81 |
| lblend25 against inpaint opposed | 34 | 15 | 54 |
| lblend25 against blend25 | 10 | 5 | 88 |
| lblend25 against opp-local | 24 | 13 | 66 |

The gains of the two add up only in part: 4 more wins and 2 more losses than blend25. The new
losses are t20, g098, g216 (each about -2 to -3); g353 drops to -11.6, g315 to -24.8.

### Tint gate on t (Gemini's research, negative)

Research round 2 (`/tmp/hl-rules-study/tasks/2026-10-24-research2/`, Gemini, `answer-gemini.md`):
five per-region or per-pixel reliability measures for the blend weight: border brightness share
times border hue concordance, Dirichlet energy of the hue field, Mertens-style border
well-exposedness, region size, and a magenta (purple-line) penalty. The papers it cites are real, but
they support only the general ideas; using them as a gate is Gemini's own inference. Region size and
border classification failed before as method selectors (issues 3, 4, 26).
Tested here: the penalty, the only new measure. `blend_gate.py`: `P = max(0, q(gd)/q(opp) - 1)`,
`q = (R+B)/(2G)` in linear Rec2020 (and the cyan analogue with `c = (G+B)/(2R)`, suffix c), blurred
4 px, `t = 0.25 exp(-P^2 / (2 s^2))`. `runs/run_gate.sh`, `pair_align-gate.json`, tables
`runs/gate-*.txt`.

| against inpaint opposed | win | loss | tie |
|---|---|---|---|
| blend25 (same run) | 30 | 13 | 60 |
| gate, s = 0.05 | 12 | 6 | 85 |
| gate, s = 0.2 | 17 | 7 | 79 |
| gate with cyan, s = 0.05 | 7 | 1 | 95 |
| gate with cyan, s = 0.2 | 14 | 5 | 84 |

Worse than a lower fixed t (0.15: 23 / 5): the gate removes wins faster than losses, all 9 phone
wins included (there, gradient domain's correction moves the hue relative to opposed's tinted
result).

### Blend with full-resolution gradient domain

`fblend25` = `opposed^0.75 * gd-mg6f^0.25` (gradient domain at full resolution, Sol's round-6
solver; `BL_B=gd-mg6f`), `runs/run_blendf.sh`, `pair_align-blendf.json`, tables `runs/blendf-*.txt`:

| comparison (103 pairs) | win | loss | tie | mixed |
|---|---|---|---|---|
| gd-mg6f (full resolution) against inpaint opposed | 21 | 56 | 23 | 3 |
| blend25 (reduced) against inpaint opposed | 30 | 13 | 60 | 0 |
| fblend25 against inpaint opposed | 34 | 12 | 57 | 0 |
| fblend25 against blend25 | 15 | 9 | 78 | 1 |

By type against inpaint opposed: city 9/5/27, colored 4/1/9, indoor 3/0/5, night 5/0/8, phone 9/0/0,
sky 2/6/4, specular 2/0/4. Alone, full resolution is worse than reduced (21 / 56 against 26 / 49);
in the blend it is better, mostly on city scenes (9 wins against 3). This is the first case where
the full-resolution mode brings something (Kofa, 2026-10-24: "we can drop it later if it does not
bring anything with any future algorithm or method"). Cost: full resolution takes up to
about 50 s CPU at 2 solver threads (issue 24).

### Full-resolution blend: weight 0.35 and the local-offsets opposed

`runs/run_blendf2.sh`, `pair_align-blendf2.json`, tables `runs/blendf2-*.txt`; win / loss against
inpaint opposed, all 103 pairs and without the 9 phone pairs:

| blend | all | without phone |
|---|---|---|
| fblend25 (opposed, full-resolution gradient domain, t = 0.25) | 34 / 12 | 25 / 12 |
| fblend35 (t = 0.35) | 37 / 20 | 28 / 20 |
| lfblend25 (opp-local, full-resolution gradient domain, t = 0.25) | 37 / 14 | 28 / 14 |

fblend35 against fblend25: 13 / 19 (6 / 19 without phone): more weight costs more than it gains.
lfblend25 against fblend25: 10 / 7 (6 / 6 without phone): opp-local adds little outside the phone
pairs.

### Without the phone pairs

The 9 phone pairs are wins for every blend, but there inpaint opposed is tinted cyan by the DNG
GainMap's false clip flags (darktable #12128, issue 2), and the blend mostly dilutes that tint
(crop sheet `fblend25-wins-sheet-part0.png`). Without them (94 pairs), against inpaint opposed:
blend25 21 / 13, gd-r-ob25 (darktable) 21 / 12, blend35 26 / 16, fblend25 25 / 12, gradient
domain alone 19 / 47 (reduced) and 13 / 55 (full).

Crop sheets for fblend25 (same layout, its own wins and losses): `/tmp/hl-rules-study/runs/
fblend25-wins-sheet-part{0,1,2}.png`, `fblend25-losses-sheet-part{0..3}.png`.
`fblend25-wins-nophone-sheet-part{0,1,2}.png`: its 10 largest wins without the phone pairs.

### Per-image rule on top of the output blend

`/tmp/hl-rules-study/tasks/2026-10-24-blend/blend_rule.py`: below an image clipped share (issue 26's
`image_clipped_share`) take one method, else another; against inpaint opposed, same run.

| rule | win / loss / tie |
|---|---|
| blend25 everywhere | 30 / 13 / 60 |
| blend50 below 2-10%, else blend25 | 33-34 / 13 / 56-57 |
| gradient domain below 4-6.6%, else blend25 | 34 / 16 / 53 |
| blend50 below 15%, else inpaint opposed | 24 / 6 / 73 |
| blend50 below 10%, else inpaint opposed | 19 / 4 / 80 |
| (issue 26) gradient domain below 10%, else inpaint opposed | 19 / 10 / 74 |

The counts are flat over 2-10%. Leave-one-scene-out (`blend_loso.py`; rule and threshold chosen
on the other scenes by wins minus losses): 33 / 13 / 57, almost always blend50 below 4%, else
blend25. That is 3 wins more than blend25 everywhere, at no extra loss.

### Crop sheets for Kofa

`/tmp/hl-rules-study/runs/blend25-wins-sheet-part{0,1,2}.png` (8 largest wins at -1 EV) and
`blend25-losses-sheet-part{0..3}.png` (all 13 losses): per pair the reference, inpaint opposed,
blend25 and gradient domain at -1 EV, cropped where blend25 differs most from inpaint opposed
(`blend_sheet.py`). The largest wins are the phone pairs (GainMap, issue 2); the losses show a
quarter of gradient domain's tint (g319: light magenta in the sky).

## Patent note

A geometric blend still uses gradient domain's hue-ratio fill (US8861851B2 claims 20, 42; see
issue 17). OFS-LIN is an additive offset transferred from a reference channel; US9912839B2
(Abebe et al., active) claims a related additive transfer (research of 2026-10-24, Sol's answer;
claims not read by me).

## Kofa's look at the crop sheets (2026-10-24)

- `fblend25` losses: the blend and gradient domain columns are "mostly unacceptable: magenta, sharp
  or 'noisy' transitions (e.g. isolated white dots on magenta background)".
- `blend25` losses: "much better, but even there" the blend and gradient domain are worse than
  inpaint opposed.
- The reference column looked corrupted (clipped, distorted colors): see below.

Visible on the wins sheet too: t39 counts as a win for fblend25 (67.0 against 50.8 at -1 EV), but
its sky has magenta speckle from full-resolution gradient domain. SSIMULACRA 2 here rewards the
blend for moving the clipped white toward the light blue of the truth, and does not penalize the
speckle enough.

Reference column: where the darker frame is clipped too, there is no truth, and pair_align.py puts
each method's own pixels into that method's reference, so those pixels do not count. The sheets
showed inpaint opposed's reference, so these areas show opposed's fill (cyan on the phone pairs, pink
on g065, white on the t39 sun). Not intended as a picture; the sheets are redrawn with these areas
in flat gray, as crop_sheet.py does.

## Verdict (Kofa, 2026-10-24)

At -3 EV (`runs/ev3-check-sheet.png`: g315, g213, g057, t39): "In every row (except maybe the 1st)
opposed looks better. Magenta is simply unnatural in the sky". Together with the -1 EV losses sheets
(see above): the blend is rejected. SSIMULACRA 2 against the truth does not penalize an unnatural hue
(magenta sky) more than a natural but wrong one (opposed's beige-gray), so its wins for the blend do
not hold up visually. The blocker is gradient domain's magenta on skies, issue 5 (parked by Kofa).

## Open (no longer pursued)

- Look at the crop sheets: is a quarter of gradient domain's correction worth its share of the tint?
- If yes: a darktable prototype of the output blend (camera space, both methods run, one parameter
  for t), scored like the others; and leave-one-scene-out for any threshold rule.

## Done when

The output blend is judged by Kofa, and either prototyped in darktable or dropped.

## Removed scratch files (2026-10-24)

`blend50.pfm`, `blend25p.pfm`, `blend50p.pfm`, `gd-r-hb25.pfm`, `gd-r-hb50.pfm` and their `bl*_*.png`
in the 103 workdirs (regenerable: `blend.py`, `run_hueblend.sh`). Then `blend15.pfm`, `blend35.pfm`,
`lblend25.pfm`, `gate*.pfm`, `fblend35.pfm`, `lfblend25.pfm` and their PNGs (`blend.py`, `blend_gate.py` regenerate them in
minutes). Kept: `blend25.pfm`,
`fblend25.pfm`, `gd-r-ob25.pfm` and their PNGs (crop sheets, pending decision).

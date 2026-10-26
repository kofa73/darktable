# Inpaint opposed: brightness of fully clipped areas

Status: pending (2026-10-25: Kofa chose option (c), issue 30, before more work here). Before: wip (2026-10-24). Kind: experiment. Kofa's direction after stopping gradient domain.

## Goal

Make inpaint opposed's fully clipped areas as bright as the scene was, keeping its hue (which Kofa
finds right) and its partly clipped pixels (whose brightness already matches the truth).

## Context

- Kofa (2026-10-24): "The problem with opposed is not the hue, it's the brightness." He prefers the
  dome versions of lamps (issue 10): "lamps are supposed to be bright".
- Measured (`/tmp/hl-rules-study/tasks/2026-10-24-brightness/deficit.py`, `deficit.json`; pixels
  the darker frame records, linear Rec2020 luminance, median per pair of log2(truth / method)):

  | | partly clipped | fully clipped |
  |---|---|---|
  | inpaint opposed | 0.0 EV (103 pairs) | 0.2 EV darker (69 pairs; quartiles 0.0 / 0.4) |
  | gradient domain (gd-r-mg4) | 0.0 EV | 0.0 EV |

  Opposed, fully clipped: g294 0.8, g065 0.6, g298 0.6, g369 0.6, t60 0.4, g213 0.4, g315 0.4,
  g057 0.3 EV. Censored: the brightest cores are clipped in the darker frame too and are not counted.
- Opposed builds a fully clipped pixel from its other channels, clipped too, plus the chroma offset,
  so it stays near the clipping level (issue 9).
- Patent (issue 17): the claims of US8861851B2 concern hue in-painting with a boundary filter,
  hue-guided gradient transfer and hue-ratio fills. A brightness-only fill is a different formulation;
  whether that matters is a legal question.

## Steps

1. Offline prototype on the existing `opposed.pfm` renders, fully clipped regions only, opposed's
   chromaticity kept:
   a. flat lift: luminance filled from the rim by a Laplace solve, raised where it exceeds opposed's;
   b. dome: `scripts/dome.py` with `DOME_SRC=opposed`, linear and log (copy in `salvage/dome.py`).
2. Score against plain opposed at -2 / -3 EV (issue 28) and measure the remaining deficit with
   `deficit.py`.
3. Crop sheets with hue-preserving display clipping (scale an over-white pixel down as a whole), so
   that brightness, not per-channel clipping, is judged: lamps, the sun, clouds, the moon.
4. If Kofa likes it: implement in `src/iop/hlreconstruct/opposed.c` behind a switch in a new
   experiment copy (the old one, `/tmp/hl-rules-study/dt-src`, was removed on 2026-10-24; build from
   upstream master), then score the real renders.

## Done when

Kofa has judged the crop sheets, and the chosen variant is scored on the 103 pairs.

## Offline prototype (2026-10-24 evening, Kofa away)

Kofa's request: try the luminance ideas of `salvage/` (linear and log dome) on inpaint opposed.
Everything is offline on the existing `opposed.pfm` renders; nothing was changed in darktable.
Directory: `/tmp/hl-rules-study/tasks/2026-10-24-brightness/`.

### Method

`lift.py` (from `salvage/dome.py`): per fully clipped region (all three channels at least half
clipped, 8-connected), the luminance is refilled from the pixels around it and the RGB scaled up
where the fill is brighter (hue kept, never darker). Variants (script names in brackets):

- flat lift (`opp-flat`): Laplace fill from the rim;
- dome (`opp-domelin`, `opp-domelog`): biharmonic fill (continues the rim's slope), linear or log
  luminance, cap 4 EV; `-s`: only regions up to 50,000 render pixels (the skies left alone);
- dome over the whole clipped area (`opp-any-domelin`, ...): partly clipped pixels refilled too,
  from the unclipped surroundings;
- robust dome (`opp-rdomelin`, `opp-rdomelog`): rim pixels darker than 0.8 x the region's median
  (occluders: trees, roofs against a clipped sky) are left free instead of fixed, so their step is
  not extrapolated (Gemini's suggestion, `../2026-10-24-lift-ideas/answer-gemini.md`); the rise
  compressed to 2 tanh(rise / 2) EV instead of the hard cap;
- height-limited domes (Luna, `../2026-10-24-lift-ideas/lift2.py`, also from Gemini's answer):
  Poisson dome (`opp-pdome*`: Laplace fill plus a constant source matched to the rim slope) and flat
  lift plus a distance-based dome (`opp-ddome*`), both with the free occluder pixels and a 2 EV limit.

Run time in Python: 0.3-1 s per image, up to 35 s on the largest skies (AMG-preconditioned CG for
regions over 40,000 pixels; not representative of C).

### Brightness deficit (`deficit2.py`, as `deficit.py`; fully clipped pixels with a truth, 69 pairs)

| variant | median deficit | quartiles | median abs. | closer to truth than opposed / farther (pairs) | overshoot > 0.5 EV (pairs) |
|---|---|---|---|---|---|
| inpaint opposed | 0.20 | 0.01 / 0.37 | 0.25 | - | 1 |
| flat lift | 0.20 | -0.01 / 0.36 | 0.25 | 1 / 1 | 1 |
| dome linear | -0.08 | -0.39 / 0.18 | 0.26 | 26 / 20 | 14 |
| dome log | -0.15 | -0.65 / 0.14 | 0.25 | 25 / 22 | 18 |
| dome linear, small regions | 0.12 | -0.17 / 0.26 | 0.23 | 18 / 8 | 3 |
| dome log, small regions | 0.08 | -0.23 / 0.23 | 0.23 | 17 / 9 | 4 |
| robust dome linear | 0.10 | -0.18 / 0.28 | 0.22 | 25 / 8 | 2 |
| robust dome log | 0.07 | -0.19 / 0.27 | 0.22 | 26 / 10 | 2 |
| Poisson dome linear | -0.00 | -0.18 / 0.26 | 0.21 | 23 / 8 | 6 |
| Poisson dome log | 0.01 | -0.18 / 0.26 | 0.20 | 27 / 5 | 3 |
| flat lift + distance dome linear | 0.06 | -0.15 / 0.27 | 0.19 | 28 / 4 | 3 |
| flat lift + distance dome log | 0.06 | -0.15 / 0.26 | 0.20 | 28 / 4 | 2 |

- The flat lift changes nothing: opposed's fully clipped pixels already sit at the level of their
  rim. The deficit is a missing rise above the rim, not a sag below it.
- On the lamps with the largest deficit the rim itself is too dark: g294's partly clipped ring is
  0.4-0.5 EV below the truth where one channel clips, so no fill from that rim reaches the truth
  (g294 0.79 EV before, 0.78 after the robust dome; t60 0.44 / 0.40). The per-pair medians of the
  partly clipped pixels (0.0 EV) hide this.
- Refilling the partly clipped pixels too (dome over the whole clipped area) makes them farther
  from the truth in 26 (linear) to 47 (log) of 103 pairs.

### Scores (SSIMULACRA 2 at -2 / -3 EV, 103 pairs, against inpaint opposed; win / loss / tie / mixed)

| variant | all | city | sky | colored | night |
|---|---|---|---|---|---|
| dome linear | 14 / 19 / 63 / 7 | 0 / 9 | 6 / 2 | 2 / 3 | 2 / 1 |
| dome log | 10 / 25 / 59 / 9 | 0 / 9 | 5 / 2 | 1 / 6 | 1 / 2 |
| dome linear, small | 9 / 11 / 81 / 2 | 0 / 4 | 3 / 1 | 1 / 2 | 2 / 1 |
| dome log, small | 8 / 17 / 77 / 1 | 0 / 4 | 4 / 2 | 0 / 3 | 1 / 2 |
| robust dome linear | 21 / 13 / 69 / 0 | 1 / 9 | 9 / 0 | 3 / 1 | 2 / 0 |
| robust dome log | 20 / 14 / 68 / 1 | 1 / 10 | 8 / 0 | 3 / 1 | 2 / 0 |
| dome linear over the whole clipped area | 2 / 79 / 21 / 1 | 0 / 31 | 0 / 12 | 0 / 13 | 1 / 6 |
| second run: robust dome linear | 21 / 13 / 69 / 0 | 1 / 9 | 9 / 0 | 3 / 1 | 2 / 0 |
| Poisson dome linear | 21 / 10 / 71 / 1 | 4 / 4 | 7 / 1 | 3 / 4 | 1 / 1 |
| Poisson dome log | 20 / 12 / 71 / 0 | 3 / 4 | 7 / 2 | 3 / 5 | 1 / 1 |
| flat lift + distance dome linear | 23 / 8 / 72 / 0 | 3 / 4 | 9 / 0 | 5 / 3 | 0 / 1 |
| flat lift + distance dome log | 23 / 10 / 70 / 0 | 3 / 4 | 8 / 1 | 4 / 4 | 1 / 1 |

Tables: `table-<variant>.txt` (first run, `pair_align-lift.json`) and `table2-<variant>.txt` (second
run, `pair_align-lift2.json`); the robust dome scored identically in both runs.

The distance dome's losses: city skies t47 (-41 / -71), t20 0715 (-19 / -15), t07 0624 (-8 / -7),
t25 0752; g193 (ceiling lamp, the sky gap; -11 / -5); t38 (white wall, -8 / -11); g437, g441 (-2).
On t20 and t07 its brightness now matches the truth (deficit 0.01-0.02 EV) and it still loses: the
fill is beige where the truth is blue, and a brighter beige differs more.

The robust dome's losses are city skies with large fully clipped regions (t47 -58 / -83, t20 0715
-49 / -62, t07 0624 -28 / -42): there the truth is only 0.05-0.2 EV above opposed and the dome
overshoots by 0.3-0.9 EV (t47: -0.89 EV). Its wins are clouds (g315 deficit 0.40 -> 0.00 EV, g213
0.39 -> 0.25, g057 0.34 -> 0.23).

### Visual check (`sheet1.png`, hue-preserving display clipping, -2 / -3 EV)

`crop_sheet.py` has two new switches: `CS_HUEKEEP=1` scales an over-white pixel down as a whole
instead of clipping each channel, `CS_SELECT=full` crops the largest fully clipped region and shows
the truth at every exposure. Default output unchanged.

- City skies (t20, t47, t07): the dome makes a white blob that follows the region's outline; clearly
  unnatural. The truth is a blue sky; opposed's beige-pink fill is the larger error there (hue).
- Cumulus clouds (g213): the domes give puffier, brighter clouds; plausible.
- Lamps (t60, g294): little visible change.

Second sheet (`sheet2.png`: opposed, robust dome linear, Poisson dome linear, distance dome linear
and log; -2 EV):

- The height-limited domes avoid the robust dome's white blobs on city skies, but the distance dome
  shows creases along the region's medial axis (t20), where the distance transform has a ridge.
- g315 (lake, glare) and g213 (cumulus): a large white glow / bright cloud where the truth is
  unrecorded or bright; plausible.
- t38 (white wall behind geraniums) and the sky gap of g193: blotchy brightening of a flat surface;
  wrong.

Lamp sheet (`sheet-lamps.png`, 29 night / indoor / light-core scenes, the unrecorded cores, -2 and
-4 EV; opposed, robust dome linear and log, distance dome linear and log): small lights (street
lamps, LED strips, windows) do not change visibly; larger lamps (t60) get a brighter center at -4
EV, strongest with the robust log dome. Most of the score gain comes from skies and clouds, not
from lamps.
### Which regions should get a dome? (Sol, `../2026-10-24-region-rule/answer.md`)

Per fully clipped region with a truth (468 regions, 58 pairs), Sol compared each dome's rise with the
truth's and tried 36 features computable from the clipped image (area, rim slope, share of free
occluder pixels on the rim, rim and region color and saturation, ...), validated leave-one-scene-out.
Measured, pixel-weighted error of the region medians: opposed 0.308 EV, robust dome linear 0.276,
distance dome linear 0.211.

- No per-region feature separates the clouds (dome right) from the city skies (dome wrong) well;
  rim saturation, for example, failed on the six key scenes when chosen on the others.
- The best gate is image-wide: dome only if more than 19% of the image is clipped (any channel);
  held-out 0.238 EV for the robust dome. Applied to the existing scores (threshold fitted on the
  same pairs, so optimistic): robust dome linear 21 / 13 -> 12 / 7, distance dome linear 23 / 8 ->
  15 / 3, distance dome log 23 / 10 -> 14 / 5 (win / loss). It removes the city-sky losses and some
  of the wins with them; the remaining losses are t25 (statue against the sky), t38 (white wall),
  g441. My reading: a fitted heuristic about framing, not a property of the regions; I would not
  build on it without more material.

### Craters from clipped specks

In the domed t20 sky the gain map has round pits: single partly clipped pixels inside the fully
clipped sky (noise) are boundary points of the fill, so the dome falls back to opposed's level
around each of them. `LIFT_HOLES=1` (`lift2.py`) merges enclosed clipped specks of up to 64 px into
the region; `LIFT_DSMOOTH=1` rounds the distance transform's ridge along the medial axis (minimum of
the distance and its blur). Together they give a smooth sky (`t20-holes.jpg`: opposed, distance
dome, with specks merged, also smoothed). Every earlier variant has the craters.

### Final variant and scores (third run, `pair_align-lift3.json`, `table3-*.txt`)

"Distance dome" from here on = `lift2.py` with `LIFT_ORDER=4 LIFT_ROBUST=0.8 LIFT_SOFT=2
LIFT_HOLES=1 LIFT_DSMOOTH=1`: per fully clipped region, a Laplace fill from the rim (occluder pixels
free) plus a dome that starts with the rim's slope and saturates at 2 EV, `Hs (1 - exp(-S d / Hs))`
with `d` the smoothed distance to the rim. Python 0.3-6 s per image.

| against inpaint opposed (win / loss / tie / mixed) | all | city | sky | colored | night | phone |
|---|---|---|---|---|---|---|
| opposed with local chroma offsets (`opp-local`, issue 9) | 14 / 7 / 82 / 0 | 1 / 3 | 1 / 2 | 2 / 1 | 3 / 0 | 6 / 0 |
| distance dome before the speck fix (`opp-ddomelin`) | 23 / 8 / 72 / 0 | 3 / 4 | 9 / 0 | 5 / 3 | 0 / 1 | 0 / 0 |
| distance dome linear (`opp-hdomelin`) | 22 / 7 / 74 / 0 | 2 / 3 | 9 / 0 | 5 / 3 | 0 / 1 | 0 / 0 |
| distance dome log (`opp-hdomelog`) | 22 / 7 / 74 / 0 | 2 / 3 | 9 / 0 | 5 / 3 | 0 / 1 | 0 / 0 |
| robust biharmonic dome with the speck fix (`opp-rhdomelin`) | 20 / 14 / 68 / 1 | 1 / 10 | 9 / 0 | 3 / 1 | 2 / 0 | 1 / 1 |
| distance dome linear on `opp-local` (`opl-hdomelin`) | 35 / 9 / 59 / 0 | 2 / 6 | 9 / 0 | 7 / 2 | 4 / 1 | 6 / 0 |

`opl-hdomelin` against `opp-local` alone: 22 / 7 / 74, the same as the dome against opposed: the
two gains add up. Deficit (fully clipped, 69 pairs): distance dome linear median 0.08 EV (opposed
0.20), closer to the truth in 25 pairs, farther in 4, overshoot > 0.5 EV in 3.

Visual check (`sheet3.png`: opposed, opp-local, distance dome linear, the same on opp-local, truth;
-2 EV, hue-preserving display):

- **opp-local's skies are pink to magenta** (t20, g213 cloud, g065): with the dome even more visible.
  By Kofa's rule (magenta skies are unacceptable whatever the score) `opl-hdomelin` is out, although
  it scores best.
- The distance dome on plain opposed keeps opposed's hue. Where that hue is right it looks
  plausible: brighter cumulus (g213), sun glare (g315), cloud rims (g057, g313). Where opposed's hue
  is wrong the dome makes the error brighter and more visible: beige city skies where the truth is
  blue (t20, t47, t07, t25), lilac where the sunset is orange (g065), a cream white wall (t38).
- Lamps: little change at -2 EV; at -4 EV larger lamps (t60) get a brighter center
  (`sheet-lamps.png`, earlier variants).

## Conclusions (2026-10-24 evening)

1. A brightness fill on opposed's fully clipped regions gains modestly in the score (22 / 7 at
   -2 / -3 EV) and halves the median deficit (0.20 -> 0.08 EV). The gain is in skies and clouds, not
   in lamps.
2. Of the salvage ideas, the biharmonic dome (linear or log) is unusable as it is: it extrapolates
   occluder edges into large white blobs in skies. It needs occluder pixels left free, a soft
   height limit and the speck fix; even then it overshoots on city skies (1 / 10). The flat lift
   alone does nothing: opposed is already at its rim's level.
3. The lamps with the largest deficit (g294 0.8 EV, t60 0.4 EV) are not helped by any fill from the
   rim, because opposed's partly clipped ring around them is itself too dark (g294: 0.4-0.5 EV below
   the truth where one channel clips). That is a separate problem: opposed's estimate of a clipped
   channel next to unclipped ones on strongly colored or bright lights.
4. Brightness exposes hue. On skies where opposed's hue is wrong (beige or lilac instead of blue or
   orange) a brighter fill looks worse even when the brightness is right. The dominant visible
   error in clipped skies is opposed's hue, not its brightness.
5. No per-region rule tells clouds from flat skies or walls (Sol); an image-wide gate (clipped share
   > 19%) works on these pairs but is fitted to them.

## Decision for Kofa

- Look at `sheet3.png` (skies, walls) and `sheet-lamps.png` (lamp cores at -2 / -4 EV; it shows the
  earlier variants; the final one differs only on large regions). Is a brighter cloud or glare worth
  a brighter wrong-hued sky?
- Options: (a) stop here (negative / too small); (b) port the distance dome to `opposed.c` behind a
  switch for real renders, on the reduced grid (Laplace solve + distance transform: cheap); (c) look
  first at the dark partly clipped ring around lamps (conclusion 3), which is where the large
  deficits are. My recommendation: (c) before (b), because without it the fill cannot reach the
  lamps that motivated this issue.

Files kept in `/tmp/hl-rules-study/tasks/2026-10-24-brightness/`: `lift.py`, `deficit2.py`,
`regstats.py`, the run scripts, logs, `deficit-*.json`, `table*-*.txt`, `score-*.log`, the sheets
`sheet1.png`, `sheet2.png`, `sheet3.png`, `sheet-lamps.png`, `t20-holes.jpg`, `t20-diag.png` (gain
map with the craters); `../2026-10-24-lift-ideas/` (`lift2.py`, Gemini's and Luna's answers);
`../2026-10-24-region-rule/` (Sol's analysis). `crop_sheet.py` in `/tmp/hl-rules-study/scripts`
has the new switches `CS_HUEKEEP`, `CS_SELECT` (original: `crop_sheet.py.orig` here). Renders kept
in the workdirs: `opp-hdomelin`, `opp-hdomelog`, `opl-hdomelin`, `opp-rdomelin`.

Removed on 2026-10-24 (regenerable in minutes with `lift.py` / `lift2.py`, settings in the run
scripts): renders `opp-flat`, `opp-domelin`, `opp-domelog`, `opp-domelin-s`, `opp-domelog-s`,
`opp-any-*`, `opp-rdomelog`, `opp-pdome*`, `opp-ddome*`, `opp-rhdomelin`; preview JPEGs of the sheets.

Copies that survive a /tmp cleanup: `tools/opposed-brightness/` (lift.py, lift2.py, deficit2.py,
regstats.py, run_lift5.sh for the final variants, run_score.sh), `tools/hl-rules-study/crop_sheet.py`
(with the new switches), `agent-notes/2026-10-24-brightness-gemini.md` and
`agent-notes/2026-10-24-brightness-region-rule-sol.md`.

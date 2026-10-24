# Keep fringe colors out of the color sources

Status: done (2026-10-23): negative for idea 1; ideas 2 and 3 not tried. Kind: experiment.

## Goal

Stop purple or green fringes around clipped areas from spreading their color over the
reconstructed area, in gradient domain and possibly in every method.

## Context

- `/tmp/hl-article/HANDOFF.md`, "Fringes as color sources" (Kofa's idea) and "Hue sources:
  selection instead of interpolation".
- Lateral CA is not the main cause on skies: rule G found 1.3-2.3 photosites of lateral CA,
  explaining < 3.5% of the sky hue error on 9 of 11 images (research HANDOFF 3.5). Most purple
  fringes come from axial CA, blooming or flare.
- Possibly the same effect: the purple patch on the birch trunk in Beelitz
  (`/tmp/gradient-domain-eval/screenshots/Screenshot_20261012_092849.png`, value 12), not reproduced.

## Ideas (untried)

1. Exclude suspected fringe pixels from the color sources: a ring of a few pixels around clipped
   areas, or pixels whose color jumps relative to their neighbors.
2. Two-sided hue: take color from the bright side of an edge only (values 10 and 11 partly do this).
3. A cleanup pass at the end of highlights, lowering chroma of reconstructed photosites whose color
   jumps relative to measured neighbors.

## To do

1. Prototype idea 1 for gradient domain; score with HANDOFF 3.6. Ground truth suits idea 1 only:
   darker frames have the same fringes at the edges.

## Done when

Known whether excluding fringe pixels changes the scores or the visible casts.

## Result (2026-10-23)

Idea 1 prototyped: `DT_GD_FRINGE=<t>` (radius `DT_GD_FRINGE_RAD`, default 4 grid pixels) in
`/tmp/hl-rules-study/dt-src` gradient.c `_gd_hue_domain()`, off by default. Under the continuous
border, a trusted border pixel is distrusted if its chromaticity rgb / (r + g + b) lies more than t
from the mean chromaticity of the pixels behind it (unclipped, not on the border, above 5% of
clipping, within the radius). On g319 at t = 0.05 it drops 304 of 12,871 donors. Script names
`gd-fringe05`, `gd-fringe10`; run `runs/run_fringe.sh`, scores `pair_align-fringe.json`, tables
`runs/fringe-*.txt`. Against plain "continuous border, hue estimate in fully clipped", 103 pairs:

| threshold | win | loss | tie |
|---|---|---|---|
| 0.05 | 5 | 15 | 83 |
| 0.1 | 4 | 2 | 97 |

- 0.05 loses mostly on skies and foliage (g315 -23.8 / -15.7, g369 -16.5 at -1 EV, q03 -16.5, g373 -13.0,
  g216 -7.8); it wins on g112 white car chrome (+14.8 / +5.6).
- 0.1 is neutral: g112 +6.7, t07 0622 +2.6, t20 +4.3 at -1 EV, g031 +3.1; q03 -5.2, g315 -2.5.
- As with the occluder test (issue 4), the trusted border is not where gradient domain's errors come
  from. Ideas 2 and 3 were not tried; idea 2 overlaps with the continuous border.
- Not checked: the Beelitz birch patch (never reproduced).

Removed on 2026-10-23 (cleanup of /tmp): renders `gd-fringe05`, `gd-fringe10`.

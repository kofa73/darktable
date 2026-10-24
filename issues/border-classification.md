# Border segments: sky versus occluder, and foliage gaps

Status: done (2026-10-23): negative; the occluder test does not help. Foliage gaps not tested.
Kind: experiment.

## Goal

Stop occluders (branches, roofs, lamp posts) from coloring clipped sky, the main remaining failure
(pink and magenta skies in g213, g319).

## Context

- `/tmp/hl-article-research-1/research/freq-domain2.md`, fixes 1 and 4; HANDOFF 3.7 items 1-2.
- Border pixel classification: look a few pixels outward along the normal. Near the clip level:
  sky, a color source (Dirichlet). Far below within 1-3 px: occluder, zero flux (Neumann).
- Differs from what failed: rule F replaced the border hue with pooled candidates; the steepness
  feature measured the edge, not what lies beyond it.
- Foliage gaps have only leaf border; fill them from the open sky (coarse to fine).

## To do

1. Cheap check on clipped skies of real pairs: color of sky segments and of occluder segments against
   the true interior color.
2. If sky segments predict well: prototype as an experiment value (behind an environment switch, in
   the copy `/tmp/hl-rules-study/dt-src/`), score with HANDOFF 3.6.

## Done when

The check shows whether the split helps; if yes, a prototype scored on the real pairs.

## Notes

- 2026-10-23, preliminary (3 pairs, while issue 2 renders): `/tmp/hl-rules-study/scripts/border_check.py`
  (per clipped region: border ring 2 px outside, classified by the darkest pixel within 2 px beyond
  it; mean border color against the aligned truth's interior color, u'v'). Large sky regions:
  - g319 (sun through trees): border pixels with >= 0.75 of the clip level beyond them: duv 0.005;
    whole border 0.026; gradient domain interior 0.035, inpaint opposed 0.007.
  - t29 0778: bright border 0.011, whole border 0.014; gradient domain 0.012, inpaint opposed 0.021.
  - g213 (clouds): almost no bright border (3 px of 6381); every border bin is 0.03-0.067 off,
    inpaint opposed 0.015, gradient domain 0.019. The clouds are whiter than anything on their
    border, so the split cannot help there; a neutral prior or the sky-trend extrapolation might.
  - Intermediate brightness (0.3-0.75 beyond) is the worst bin in all three: probably blue sky and
    fringes. The thresholds of the article (near clip versus far below) leave these out.
  - A single mean color per region hides local errors (g213's pink patches); a local version
    (interior pixels near each border segment) is the next refinement.

## Step 1 result (2026-10-23): positive for daylight skies

On the 103 pairs of issue 2's table (`runs/allpairs-border-summary.txt`, `runs/allpairs-border-local.txt`;
scripts `border_check.py`, `border_local.py`). Thresholds: bright >= 0.75 of the clip level within
2 px beyond the border pixel, occluder < 0.15, mid between.

1. Per region (280 regions >= 5000 px): the brighter the image beyond a border pixel, the better it
   predicts the true interior color. Median u'v' error by brightness beyond: < 0.05: 0.023,
   0.05-0.15: 0.018, 0.15-0.3: 0.016, 0.3-0.5: 0.013, 0.5-0.75: 0.010, >= 0.75: 0.005. Bright border
   against the whole border: 0.005 against 0.010 (better in 72% of regions); regions with >= 20%
   occluder border: 0.008 against 0.016. But both methods' mean interior color is already about as
   good (inpaint opposed 0.003, gradient domain 0.004): the failures are local.
2. Per pixel, by the class of the nearest border pixel, distance band, and partial or full clipping;
   median / 90th percentile u'v' error against the truth. Partly clipped pixels, daylight scenes:

   | type, nearest border | 2-10 px | 25-60 px | 150+ px |
   |---|---|---|---|
   | sky, bright: opposed / gd | .0024/.0050, .0025/.0053 | .0025/.0056, .0033/.0155 | .0032/.0086, .0060/.0169 |
   | sky, occluder: opposed / gd | .0111/.0308, .0135/.0383 | .0128/.0350, .0207/.0480 | .0065/.0105, .0358/.0914 |
   | city, bright: opposed / gd | .0020/.0041, .0022/.0049 | .0024/.0055, .0031/.0094 | .0025/.0050, .0041/.0092 |
   | city, occluder: opposed / gd | .0047/.0161, .0061/.0256 | .0033/.0172, .0047/.0421 | .0027/.0125, .0047/.0315 |

   Gradient domain's blotches (p90) are 2-5 times larger where the nearest border is an occluder,
   and in sky scenes grow with distance from it; inpaint opposed's do not. Colored scenes show no
   such pattern. Fully clipped pixels behave differently (gradient domain's error there comes from
   its hue estimate, not from the border; it beats opposed on city and specular).
3. Correlation, not proof: occluder-adjacent sky may be harder in other ways. The prototype (step 2)
   decides.

## Step 2 result (2026-10-23): negative

The experiment value "continuous border, hue estimate in fully clipped" (the gradient domain
variant scored everywhere, script name `gd-wb4x10`) already distrusts border pixels
that jump from their clipped neighbor: they become unknowns in the hue solve and are left out of the
channel solves and the model fit (`_gd_hue_domain()`, `_gd_continuous()`), which is close to fix 1's
zero flux. It compares a border pixel only with its clipped neighbor, so a sky pixel mixed with the
leaves behind it passes. The prototype adds the article's look outward:

- `DT_GD_OCCLUDER=<t>` (and `DT_GD_OCC_RAD`, default 5 grid pixels) in
  `/tmp/hl-rules-study/dt-src/src/iop/hlreconstruct/gradient.c`, `_gd_hue_domain()`: a trusted
  border pixel is distrusted if an unclipped pixel within the radius has its largest channel below
  t times its clipping level. Off by default; built into `/tmp/hl-rules-study/dt-build`.
- Script method names `gd-occ15` and `gd-occ30` (the plain variant plus the switch at t = 0.15 and
  0.3) in `scripts/ev4.py`; rendered into the 67 daylight pairs of
  issue 2 (city, colored, sky) with `scripts/render_extra.py` (`runs/run_occ.sh`, `runs/occ.log`),
  scored with pair_align.py on all four methods. Tables: `runs/occ-table-gd-occ15.txt`,
  `runs/occ-table-gd-occ30.txt`.

| comparison (67 daylight pairs) | win | loss | tie |
|---|---|---|---|
| t = 0.15 against plain "continuous border, hue estimate in fully clipped" | 4 | 4 | 59 |
| t = 0.3 against the plain variant | 5 | 13 | 49 |
| t = 0.15 against inpaint opposed | 3 | 49 | 15 |
| plain variant against inpaint opposed (same run) | 3 | 50 | 14 |

Largest changes at 0.15: t07 0622 -12.9 / -12.1, g315 -11.9 / -7.7; t42 0867 +6.5 / +5.5, t32 0797
+6.5 at -1 EV. On g319 the pink sky is unchanged (`/tmp/hl-rules-study/scratch/occ/g319-occ.png`).

Why step 1 misled: the correlation with occluder borders holds, but the error is not the occluders'
color leaking in. In g319's pink area green (37%) or green and blue (54%) are flagged, and the solve
leaves green near its clipped input (median 0.88 against 1.79 in the truth; inpaint opposed 1.72).
Large clipped skies bordered by trees are simply where such areas are. That is issue 5
(partly-clipped-sky-tint.md), which now carries the main daylight losses.

Foliage gaps (fix 4) were not tested: their premise is the same leakage of leaf color, which the
results above do not support as the main error. Revisit only if issue 5 is fixed and small gaps
between leaves still look wrong.

Removed on 2026-10-23 (cleanup of /tmp): renders `gd-occ15.pfm`, `gd-occ30.pfm` and their pair_align PNGs (re-render with `scripts/render_extra.py`; the switch stays in dt-src); the g319 copy in `scratch/occ/` except `g319-occ.png`.

# Inpaint opposed with local chroma offsets

Status: done (2026-10-23): modest gain over inpaint opposed (16 wins, 6 losses); lamps' fully
clipped cores not helped. Kind: experiment.

## Goal

Keep inpaint opposed's behavior on skies while fixing its failures on colored lights.

## Context

- Inpaint opposed estimates one chroma offset per channel for the whole image
  (`src/iop/hlreconstruct/opposed.c:356-376`), from unclipped photosites next to clipped ones.
  That suits large neutral areas and fails on colored lights (HANDOFF 3.4, 3.8).
- Not gradient domain, so no Rouf/Dolby patent question.

## To do

1. Estimate the offset per connected clipped region (or per tile), with the global offset as fallback
   where a region has too few border samples or a heterogeneous border.
2. Prototype in the copy `/tmp/hl-rules-study/dt-src/` behind an environment switch; score with
   HANDOFF 3.6 on the real pairs, lamps and skies both.

## Done when

Scored against plain inpaint opposed and gradient domain on the real pairs.

## Result (2026-10-23)

Prototype: `DT_OPP_LOCAL=1` in `/tmp/hl-rules-study/dt-src/src/iop/hlreconstruct/opposed.c`,
`_process_opposed()` only (raw CPU path; not the linear-raw path, not OpenCL); off by default. Patch
script: `/tmp/hl-rules-study/scratch/patch_opp_local.py`. On opposed's 3x3 mask grid, each
8-connected region of cells with any clipped channel gets a label, grown 3 cells into the
surroundings (the reach of `_mask_dilated()`); per region and channel, the offset is the mean of
opposed's usual samples (unclipped, between 0.2 and 1 times clipping) within that region's reach; 30
samples or fewer fall back to the global offset. Script name `opp-local` (`scripts/ev4.py`).
Run: `runs/run_opplocal.sh`, scores `pair_align-opplocal.json`, tables `runs/opplocal-*.txt`.

| comparison (103 pairs) | win | loss | tie | mixed |
|---|---|---|---|---|
| local offsets against inpaint opposed | 16 | 6 | 81 | 0 |
| local offsets against gradient domain (continuous border, hue fill) | 61 | 22 | 19 | 1 |

By type, against inpaint opposed: city 0/0/41, colored 2/1/11, indoor 2/1/5, night 4/0/9, phone 7/1/1,
sky 1/3/8, specular 0/0/6.

- Gains (0 / -1 EV): phone q08 +18.0 / +24.0 (both pairs), q00 +16.5 / +3.6, q03 +5.5 / +2.0; night
  g300 +10.0 at -1 EV, t57 +6.9 and +5.8, n041 +2.2; t51 purple neon +6.7 and +6.6; t46 +2.1, +3.7.
  On the phone DNGs (lens-shading gain maps, issue 2) the clip level varies across the frame, and a
  per-region offset follows it.
- Losses: t60 wall lamp -13.3 at -1 EV, g098 -8.0, g369 -3.3, p04 -4.3, g315 -2.7, g213 -1.6 / -2.2.
- City skies are all ties: the daylight behavior of inpaint opposed is kept.
- What it cannot do: fully clipped lamp cores. Opposed builds a fully clipped pixel from its other
  channels, clipped too, plus the offset, so it stays near the clip level. t60 0034, fully clipped
  median: truth (5.68, 3.03, 7.48), opposed (2.04, 2.33, 4.58), local (1.86, 2.35, 4.57), gradient
  domain (3.43, 3.42, 4.18). g294 is unchanged (45.8 against 45.7 at -1 EV; gradient domain 78.7).
- Run time: 3-15 s per render (one job, 3-12 threads), like plain opposed.

Open: the t60 and g098 losses (few samples per region? a heterogeneous border?); a minimum count or
border homogeneity test; the OpenCL and linear-raw paths. Being opposed, not gradient domain, it
raises no patent question.

Lost on 2026-10-24: the prototype code (`DT_OPP_LOCAL` in `/tmp/hl-rules-study/dt-src`, and its patch
script `scratch/patch_opp_local.py`) was deleted in the /tmp cleanup; it was never committed. The
description above (3x3 mask grid, 8-connected regions grown 3 cells, per-region mean of opposed's
samples, fallback to the global offset at 30 samples or fewer) is enough to rewrite it. The renders
`opp-local.pfm` in the 103 pair workdirs remain.

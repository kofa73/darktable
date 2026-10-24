# Apply a clipped channel's lower bound to the hue estimate in all flagged pixels

Status: done (2026-10-23): negative; the bound fixes neither the specks nor the pink skies.
Kind: experiment.

## Goal

Keep the hue estimate consistent with each pixel's own data: a clipped channel is at least its
clipping level, so the ratio to a measured channel has a lower bound.

## Context

- `/tmp/hl-article/HANDOFF.md`, "The hue estimate ignores the bound of a clipped channel".
- On the fire strip, red clipped (>= 1.0) with green 0.08 means red/green >= 12.5, but the
  interpolated flame hue says 3. That produced the green specks `4acc6630c6` repairs afterwards.
- The model hue already applies the bound for pixels with exactly one flagged channel
  (`src/iop/hlreconstruct/gradient.c:1093`). Pixels with two or three flagged channels, and regions
  without an accepted model, keep the interpolated hue unbounded.
- Kofa's variant: carry the ratio in from the dark edge, `red[2] = green[2] / green[3] * red[3]`;
  a bound only at the edge pixel.

## To do

1. Prototype the bound for all flagged pixels (and Kofa's variant) as experiment values.
2. Check the fire (with holding off, to see whether the specks go), DSC_5399, and the real pairs
   with HANDOFF 3.6.

## Done when

Known whether the bound fixes the specks and the pink shifts at the source; feeds
`holding-and-writeback.md`.

## Result (2026-10-23)

Prototype: `DT_GD_HUE_BOUND=1` in `/tmp/hl-rules-study/dt-src` gradient.c, after all hue sources and
before the channel solves (off by default). In every partly clipped pixel, for each flagged channel
j and each measured channel k at or above the dark level (5% of clipping): rho_j is raised to
rho_k * v_j / v_k where it is lower. Script names in `scripts/ev4.py`: `gd-bound` (current code
plus the bound), `gd-bound-nohold` (also `DT_GD_NO_HOLD`), `gd-r-bound-nohold` (the same at reduced
resolution). Base: "continuous border, hue estimate in fully clipped", write-back "joint, any
channel clipped, capped".

1. Fire (`DSC_0028`), reduced resolution, no holding: the bound is active (75,065 grid pixels
   raised, 6% of the render changed, `-d perf`), but the green specks at the child's head and the
   flame edge stay (`runs/hold2/singles/2018-06-23_22-34-52_DSC_0028-bound-sheet.png`: holding, no
   holding, bound without holding). The specks come from the missing anchor (the strip's hair side is
   left out of the solve), not from the hue ratio; holding at the input fixes that directly.
2. Three pairs (`pair_align-bound.json`), SSIMULACRA 2 at 0 / -1 EV:

   | pair | opposed | holding | no holding | bound | bound, no holding |
   |---|---|---|---|---|---|
   | t39 0848 (pink sky) | 92.3 / 50.8 | -1.6 / 1.2 | 52.3 / 10.3 | -1.6 / 1.3 | -4.1 / 0.7 |
   | g319 0292 (pink sky) | 66.3 / 53.4 | -33.3 / -51.9 | 24.0 / -33.6 | -29.5 / -50.9 | -26.2 / -49.0 |
   | g294 0122 (lamp) | 82.3 / 45.7 | 85.2 / 78.7 | 85.0 / 78.4 | 85.1 / 78.4 | 84.9 / 78.3 |

   The bound leaves the pink skies as they are, and with no holding it undoes most of what dropping
   holding gained there. Lamps unchanged. Not run on all 103 pairs: no effect where it was expected.
3. Kofa's variant (carry the ratio in from the dark edge, a bound at the edge pixel only) was not
   prototyped: it is a weaker form of the per-pixel bound above, which already fails on the fire.

Consequence for `holding-and-writeback.md`: the bound cannot replace holding at reduced resolution.

Removed on 2026-10-23 (cleanup of /tmp): renders `gd-bound`, `gd-bound-nohold`.

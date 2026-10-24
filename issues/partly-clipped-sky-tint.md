# Partly clipped, near-neutral areas get a magenta or cyan tint

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: pending (parked 2026-10-23 by Kofa's choice: lamps keep "continuous border"; see Results). Kind: bug (experiment to fix).

## Goal

Stop gradient domain from tinting bright, near-neutral areas (clouds, pale skies) where only some
channels are clipped. No fill rule for fully clipped pixels reaches these pixels.

## Context

- Design notebook `/tmp/hl-article/HANDOFF.md`, "Partly clipped, nearly neutral areas get a tint"
  and "DSC_5399: pink in an eye white, a green mortar line"; `/tmp/hl-article-research-1/HANDOFF.md`
  3.5 ("New"), 3.7 item 3.
- Cases: Half Dome `_MDF0060` against `_MDF0058` (no fully clipped pixels; mean dE inpaint opposed
  1.7, gradient domain 6.1; crop `/tmp/hl-rules-study/halfdome/p60-sky.png`); TZ101 t29 clouds next
  to blue sky (`/tmp/hl-rules-study/runs/t29-crop.png`); SI-HDR 145 (green raised 30% where the truth
  is 2% above); sky_g281 and `DSC_0535.NEF` (magenta cast); DSC_5399 eye white and mortar line.
- Cause on sky_g281 (Codex, stage dumps in `/tmp/claude-gd-artefacts/codex/work/opinion-1955/`):
  border colors are right, but the cloud's hue changes with brightness, and interpolating the
  border cannot follow it. Solved green is 6.7% low at the median. Fitted relations predict better:
  G = a R + b B on the border -2.5%, opposed-like cube-root offset -1.6%
  (`/tmp/claude-gd-artefacts/tint/estimators.py`).
- On DSC_5399 the cause is channel levels: the solve uses the hue only as a ratio of changes, so
  each channel's level comes from its own boundary values.

## Candidate fixes (untried)

1. Hue estimate as a target value in partly clipped pixels: pull the clipped channel toward the
   value the measured channels imply (fitted relation above). "hue estimate in fully clipped" does
   this for fully clipped pixels only.
2. Extrapolate the region's color trend against log brightness, measured on the unclipped border
   (freq-domain2.md fix 3; Codex idea 5 in `/tmp/claude-gd-artefacts/codex/2026-10-12-06-02-codex-ideas.md`).

## To do

1. Prototype 1 and 2 as experiment values in `/tmp/hl-rules-study/dt-src/`.
2. Score on the real pairs with HANDOFF 3.6 (Half Dome p60, g213, g112, t29) and check the lamps
   (g294, t60) do not regress; look at DSC_5399.

## Done when

Either variant removes the tint on the listed skies without losing the lamps, or both are shown not
to, with scores and crops.

## Notes

- 2026-10-23, from issue 4's prototype (g319 sun through trees, `allpairs/g319-sun-through-trees-0292-0290`):
  the pink area (gradient domain's G / mean(R, B) below 0.8 of the truth's; 38% of the clipped
  pixels) has green alone flagged in 37% of its pixels and green and blue in 54%. Median linear RGB
  there: truth (1.49, 1.79, 1.98), none (1.41, 0.76, 2.35), inpaint opposed (1.53, 1.72, 2.06),
  gradient domain (1.42, 0.88, 2.28). Green stays near its clipped input: the solve does not lift
  it, and holding at the input keeps it there. Distrusting border pixels with an occluder behind
  them (issue 4) leaves this unchanged (`/tmp/hl-rules-study/scratch/occ/g319-occ.png`).
- Issue 2's table: these daylight skies are where gradient domain loses most (t39, t27, t42, g213,
  g319), so this issue carries the largest share of the losses.
- 2026-10-23, step 1 started: scoring the existing experiment value "opposed's estimate as hue, hue
  estimate in fully clipped" (script name `gd-wb4x9`; described in
  `dev-doc/iop/highlight/gradient_domain.md`, "Experiment values", and in `/tmp/hl-article/HANDOFF.md`,
  "Experiment 6 (value 9)") on the 103 pairs of issue 2, against "continuous border, hue estimate in
  fully clipped" (`gd-wb4x10`) and inpaint opposed. It gives every partly clipped pixel, including
  those with two flagged channels, the hue of inpaint opposed's estimate. Only scored on mean dE so
  far. Run: `/tmp/hl-rules-study/runs/run_x9.sh`, log `x9.log`, scores `pair_align-x9.json` per workdir.

## Results so far (2026-10-23)

Names: CB = "continuous border, hue estimate in fully clipped" (script `gd-wb4x10`, the variant of
issues 2-4); OE = "opposed's estimate as hue, hue estimate in fully clipped" (`gd-wb4x9`).

1. OE on the 103 pairs (`runs/x9-vs-opposed.txt`, `x10-vs-opposed.txt`, `x9-vs-x10.txt`;
   scores in `pair_align-x9.json`). OE against CB, wins / losses / ties: city 24/6/11, sky 10/0/2,
   colored 6/4/3, indoor 2/3/3, night 2/6/4, phone 2/7/0, specular 3/2/1. OE against inpaint
   opposed: 13/39/50, median difference 0.0 at 0 EV and -0.3 at -1 EV: OE mostly reproduces inpaint
   opposed, including its phone failure, and gives up gradient domain's night and lamp wins.
   It removes the pink partly clipped skies: t39 0848 0 EV CB -1.6, OE 92.2 (opposed 92.2); green in
   the formerly pink area 1.87 against 1.00 (truth 1.64; `/tmp/hl-rules-study/scratch/t39-x9.png`).
2. OE's remaining sky losses are at -1 EV, in fully clipped areas: g369 has 793 k fully clipped
   pixels, which OE fills pink (median 1.94, 1.33, 2.33; 93% of them with R and B > 1.1 G). Correction
   (2026-10-23, found by GPT-6.1 Sol, verified): CB fills the same sky near neutral (2.05, 2.09, 2.11;
   5% pink); inpaint opposed 2.04, 1.98, 2.10; truth 2.75, 3.14, 4.68. Both use "hue estimate in fully
   clipped", so the fill formula is not the cause: without the continuous border, OE's hue estimate
   is fixed by all border pixels, which are green-deficient here (median camera RGB 0.73, 0.59, 0.65),
   while CB keeps 346 of 117,369 as donors. Report: `/tmp/hl-rules-study/tasks/2026-10-23-pink-fill/answer.md`.
   An earlier version of this note blamed the fill itself.
3. Prototypes in `/tmp/hl-rules-study/dt-src` gradient.c `_gd_opposed_hue()`, off by default:
   - `DT_GD_OPP_TWO=1` (script `gd-opp2`): under CB, opposed's estimate as hue only for pixels with
     two flagged channels (CB's model hue covers one flagged channel).
   - `DT_GD_OPP_LOCAL=1` (`gd-opp2l` with the above, `gd-x9l` with OE): one opposed offset per
     4-connected clipped region, from opposed's kind of samples within 2 sigma_s of that region;
     the global offset where a region has 20 samples or fewer.

   Four pairs (`pair_align-opp.json`), SSIMULACRA 2 at 0 / -1 EV:

   | pair | opposed | CB | OE | gd-opp2 | gd-opp2l | gd-x9l |
   |---|---|---|---|---|---|---|
   | t39 0848 (sky) | 92.2 / 50.8 | -1.6 / 1.2 | 92.2 / 50.3 | 86.7 / 39.8 | 86.5 / 38.3 | 92.2 / 51.3 |
   | g319 0292 (sky) | 68.4 / 54.6 | -33.2 / -51.9 | 56.9 / -8.9 | -2.1 / -44.5 | -4.5 / -45.0 | 54.2 / -10.8 |
   | g294 0122 (lamp) | 82.3 / 45.7 | 85.3 / 78.6 | 82.4 / 44.8 | 82.7 / 45.0 | 82.4 / 43.8 | 82.3 / 43.7 |
   | t60 0034 (lamp) | 88.4 / 70.9 | 88.4 / 80.5 | 88.3 / 76.4 | 88.4 / 75.1 | 88.4 / 72.9 | 88.4 / 74.2 |

   The lamp wins depend on the border hue in pixels with two flagged channels: any opposed estimate
   there loses g294 (78.6 -> 44-45). Local offsets do not save it, because opposed's reference for
   such a pixel mixes the one measured channel with a clipped one. The two hue sources trade lamps
   against skies; none of the variants keeps both.
4. Render times (3 threads, 4 jobs): t39 89-206 s, g319 77-158 s per variant; lamps 14-43 s.
5. Not tried: candidate 2 (color trend against log brightness), and a per-region selector between the
   border hue and opposed's estimate.

Removed on 2026-10-23 (cleanup of /tmp): renders `gd-opp2`, `gd-opp2l`, `gd-x9l` (four pairs; switches stay in dt-src); Sol's GDB dumps and build outputs in `tasks/2026-10-23-pink-fill/` (regenerate with its `reproduce.sh`; `answer.md` and `analysis.json` kept). `gd-wb4x9` renders kept for this parked issue.

## From the paper (Rouf, Lau, Heidrich 2012, `/tmp/hl-article/Rouf2012GDC.txt`, read 2026-10-23)

- This failure is the method's stated limit. Section 3: "The fundamental assumption ... is that the
  hue varies smoothly over Ω∪ and can be estimated from the pixels on its boundary"; valid "for
  highlights generated by a single colored light source such as an LED or a neon sign", "violated
  for scenes such as sunsets where the hue of the sky may not be independent of the luminance".
  Figure 6 is a cloud failure case; the conclusion repeats it and suggests learning the
  hue-intensity relation from other images of similar scenes.
- Pixels with two clipped channels are the paper's primary case (eq. 4, f*_j = ρ_j/ρ_k f_k with ρ from
  the boundary), so the pink skies follow from the design, not from a deviation in our code.

## From Masood, Zhu, Tappen 2009 (read 2026-10-23)

"Automatic Correction of Saturated Regions in Photographs using Cross-Channel Correlation", Pacific
Graphics 2009 (Computer Graphics Forum 28(7)); `/tmp/hl-article/Automatic Correction of Saturated
Regions in Photographs using Cross-Channel Correlation.pdf` (from Kofa) and the text extract `.txt`.
- Two quadratic minimizations (colorization style): channel ratios propagated into the saturated area
  with neighbor weights from similarity in the unsaturated channels (eq. 1-2, 3x3, local variance as
  sigma), then each saturated channel from ratio x measured channel, smoothing only where all channels
  are saturated (eq. 3).
- Difference from Rouf: Rouf's hue is a uniform Laplace interpolation of the border; Masood's
  propagation follows the structure the measured channels still show inside the region, so colors do
  not flow across edges visible in the unclipped channels (the occluder leakage that the continuous
  border handles only at the border).
- Does not cure this issue: the ratios still come from unsaturated neighbors, so a hue that changes
  with brightness (sunlit clouds, Rouf's Figure 6) defeats it too. Listed failures: fully saturated
  regions; saturated edges between contrasting colors (yellow petals against grass).
- Cost: a spatially varying sparse solve (30 s for 500x400 in MATLAB); the convolution pyramid
  shortcut of issue 15 does not apply.
- Candidate (untried): an edge-aware hue estimate steered by the measured channels for pixels with
  one or two flagged channels, instead of the Laplace fill. Rouf et al. report Masood comparable on
  their neon sign and worse at boundaries between regions with different numbers of clipped channels.

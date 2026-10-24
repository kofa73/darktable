# Choose the method per region: inpaint opposed for skies, gradient domain for lights

Status: done (2026-10-23): no gain per region; rerun after issues 4 and 5. Kind: experiment.

## Goal

Find out whether picking the reconstruction method per clipped region beats either method alone.

## Context

- Gradient domain wins on compact light sources and loses on extended near-neutral areas (skies,
  clouds next to trees); inpaint opposed is the reverse (HANDOFF 3.6, 3.8).
- The rules study chose between gradient domain's fills, never between methods
  (`/tmp/hl-rules-study/REPORT.md`, scored on mean dE, which proved unreliable).
- Per-region composites of two renders are cheap: `regions.py` and `analyze.py` in
  `/tmp/hl-rules-study/scripts/` already build zone maps; `pair_align.py` scores seams.

## To do

1. On the scored real pairs, build composites: per connected clipped region, take the render of
   inpaint opposed or gradient domain.
2. Oracle first: the best choice per region by the score. If the oracle barely beats the better
   single method, stop.
3. Then simple rules: region size, compactness, share of the border near the clip level (sky)
   versus far below it (occluder), saturation trend toward the core.
4. Check seams visually; composites may show edges a real per-region method would avoid.

## Order relative to issues 4 and 5 (Kofa's question, 2026-10-23)

The oracle (steps 1-2) is offline: composites of the two methods' own full-image renders, no code
change, so gradient domain stays available and scorable alone on skies for issues 4
(border classification) and 5 (partly clipped sky tint). Rules (step 3) wait until after 4 and 5:
a rule fitted on today's gradient domain, which fails on skies, would be invalidated if 4 or 5
fixes skies. Rerun the oracle with the improved gradient domain then. Script:
`/tmp/hl-rules-study/scripts/region_oracle.py` (writes `oracle.json`, `oracle_choice.png`).

## Done when

Oracle gain known; if worthwhile, a rule tested on held-out scenes and checked by Kofa.

## Result (2026-10-23)

`region_oracle.py` on the 103 pairs included in issue 2's table (PA_HALF=1), log
`/tmp/hl-rules-study/runs/allpairs-oracle.log`, summary `allpairs-oracle-summary.txt`. Regions are the
8-connected components of pair_align.py's changed mask; regions of >= 200 px with truth pixels are
scored locally (SSIMULACRA 2 on a crop, sum of 0 and -1 EV), smaller ones take the pair's better
method. The composite is scored on the whole image like pair_align.py.

- Oracle minus the better single method of the same pair: median +0.0 at 0 and -1 EV, in every scene
  type. Gains >= 2 on 4 of 103 pairs only: g117 whitewall tire +9.7 at -1 EV, g120 chrome trim +3.3,
  g291 office windows +2.6, g298 atrium lights +2.6. t57 (both pairs) is 4 worse at 0 EV: the local
  crop score and the whole-image score disagree there, a measure of the oracle's own noise.
- Within one image, one method wins almost every region: gradient domain's median share of the
  changed pixels is 1% on sky, 2% city, 24% colored, 56% indoor, 82% night, 98% phone.
- The gain is per image, not per region: oracle minus inpaint opposed, median at -1 EV, +3.8 on night
  scenes, +35 on phone pairs (where opposed fails, see issue 2), 0 elsewhere.
- Limitation: connected components cannot separate a lamp that touches a clipped sky; such cases
  were not looked for.

Conclusion: choosing the method per region does not beat choosing per image on these pairs; step 3
(rules) is not worth doing now. Rerun the oracle once issue 4 or 5 changes gradient domain on skies.

# Segmentation based with automatic combine and candidating

Status: done (2026-10-23): oracle gain small outside phone pairs; no rule sought. Kind: experiment.

## Goal

Find a per-image rule for segmentation based's **combine** and **candidating** sliders, since tuning
helps where the defaults do nothing.

## Context

- Research HANDOFF 3.8 item 4; `/tmp/hl-rules-study/2026-10-21-log.md`, "Moderate clipping, 12 scenes".
- Kofa tunes them per image with the combine and candidating masks; every score in the study used
  the defaults (combine 2, candidating 0.4).
- Sweep (combine {1,2,4} x candidating {0.2,0.5,0.8}, best chosen by the truth): mean dE 3.89 against
  4.54 at defaults on 21 cases; magenta dahlia 13.5 -> 4.6 at combine 4, candidating 0.8. An upper
  bound, on mean dE.
- Not gradient domain, so no patent question.

## To do

1. Repeat the sweep on the real pairs, scored with HANDOFF 3.6.
2. If the oracle gain holds, look for image features that predict the best setting.

## Done when

Oracle gain known with the new score; if worthwhile, a rule tested on held-out scenes.

## Result (2026-10-23)

Segmentation based at its defaults (combine 2, candidating 0.4; script name `segments`) and the sweep
combine {1, 2, 4} x candidating {0.2, 0.5, 0.8} (`seg-c<combine>-k<10 x candidating>`, HEAD code) on
the 103 pairs of issue 2 (`runs/run_seg.sh`, scores `pair_align-seg.json`, oracle
`runs/seg-oracle.txt` from `scripts/seg_oracle.py`, tables `runs/seg-*.txt`). About 4 s per render.

- At the defaults, segmentation based scores the same as inpaint opposed on almost every pair.
- Per-image oracle (best setting chosen with the truth, sum of 0 and -1 EV) against the defaults:
  median +0.0 / +0.2, mean +0.6 / +2.2 (0 / -1 EV); a gain of 2 or more on 19 of 103 pairs, 6 of them
  phone pairs (p04 +26.6 / +57.2, q07 +13.9 / +18.8 and +7.7 / +26.2, q03 +10.2 / +6.1). Elsewhere:
  g300 +14.7, t60 0034 +10.5, t51 +10.0 and +5.1, t39 +6.6 at -1 EV; city, colored, sky, specular means
  +0.5 to +1.3 at -1 EV.
- Best setting counts: combine 4 / candidating 0.8 32, defaults 27, combine 1 / candidating 0.8 14,
  others 30.
- No fixed setting beats the defaults: combine 4 / candidating 0.8 wins 17, loses 16 (g065 -31.4, q08
  -24.9 and -25.8, g294 -10.8 at -1 EV); combine 1 / candidating 0.8 10 / 8; combine 4 / candidating
  0.5 4 / 4.
- The oracle gain sits mostly on the phone DNGs, where inpaint opposed and segmentation based fail
  (issue 2), and even among them the best setting differs (q08 loses 25 with the setting that helps
  q07). Outside the phone pairs, the mean gain is 0.5-2.3 at -1 EV: not worth a per-image rule.
  Inpaint opposed with local offsets (issue 9) gains on the same phone pairs without any slider.

Removed on 2026-10-23 (cleanup of /tmp): the sweep renders (`segments`, `seg-c*-k*` PFMs, sidecars, pair_align PNGs); scores and tables kept.

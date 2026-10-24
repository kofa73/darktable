# Directional 1D declipping along four directions

Status: done (2026-10-23): step 1 negative; disagreement is a weak error predictor. Kind: experiment.

## Goal

Test whether reconstructing clipped runs along rows, columns and both diagonals, combined with a
weighted median, is useful as a separate method or as an error predictor.

## Context

- Research HANDOFF 3.7 item 7 and 3.8 item 3; `/tmp/hl-article-research-1/research/freq-domain1.md`,
  `Directional 1D Raw Highlight Declipping.md` (their numbers unchecked).
- The articles agree a frequency prior adds spatial shape, not color: it will not decide white
  against colored by itself.
- On Bayer, diagonals alternate R and B and stay unclipped where only green clips.
- The third-difference fit for cores is tracked in `dome-for-clipped-cores.md`.

## To do

1. Cheap first test: on the real pairs, does disagreement between directions predict where
   gradient domain or inpaint opposed is wrong?
2. Only if it does, prototype the method offline and score with HANDOFF 3.6.

## Done when

Step 1 answered.

## Step 1 result (2026-10-23)

`/tmp/hl-rules-study/scripts/dir_declip.py`, offline on the clipped frame's raw (Bayer). Per CFA plane
and direction (row, column, both diagonals), each clipped run of at most 40 plane samples with two
unclipped samples on each side gets a least-squares quadratic through those four samples, raised to
the clipping level inside the run. Disagreement = standard deviation of the log estimates over the
directions that have one (at least 2), largest over the planes of a 2x2 quad, registered onto the
render as ev2.py does. Compared per clipped render pixel (with a truth, truth Y >= 0.05) with the
errors of inpaint opposed and "continuous border, hue estimate in fully clipped" against the aligned
truth (|log2 Y ratio| and u'v'), by Spearman correlation; baseline: distance to the nearest
unclipped pixel; "dis|dist" = the disagreement's correlation within distance bands (0-3, 3-8,
8-20, 20+ px), i.e. what it adds beyond distance. 103 pairs of issue 2, about 3.5 s per pair.
Summary `runs/dirdeclip-r40-summary.txt`, per pair `dir_declip-r40.json`, log `runs/dirdeclip-r40.log`.

- Coverage: the disagreement exists for a median 17% of the scored clipped pixels (city 4%, sky 10%,
  night 45%, indoor 46%): long runs in large regions have no estimate.
- Median Spearman over pairs, all types: inpaint opposed, luminance error +0.12 (within distance
  bands +0.09), color error +0.13 (+0.13); gradient domain, luminance +0.06 (+0.07), color +0.13 (+0.10).
  Distance alone: -0.03 to +0.11. Highest on phone pairs (opposed color +0.46, distance +0.52) and
  specular (+0.21 to +0.26).
- Answer to step 1: weakly. Correlations of about 0.1 on the pixels it covers are not enough to
  steer a method choice or a per-pixel blend, so step 2 (a directional method) was not started.
- With runs up to 200 samples (`DD_RUNMAX=200`, `runs/dirdeclip-r200-summary.txt`): coverage 39%
  (city 12%, sky 24%, night 66%), correlations unchanged: inpaint opposed +0.13 / +0.19 (luminance /
  color; within distance bands +0.09 / +0.11), gradient domain +0.06 / +0.11 (+0.07 / +0.11). The
  conclusion holds.

# Choose inpaint opposed or gradient domain per image by a self-test

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: pending (2026-10-24, Kofa's decision): self-test negative; a per-image switch on the clipped share beats both methods; per-region switching does not. Kind: experiment (new idea).

## Goal

Pick the better of inpaint opposed and gradient domain per image, automatically, from the clipped
image alone. Issue 3's oracle shows that the gain is per image, not per region (better method per
image against inpaint opposed: +3.8 median at -1 EV on night scenes, +35 on phone pairs, 0
elsewhere).

## Idea

Lower the clipping threshold (factor f < 1): pixels just below the real clipping level become
synthetically clipped, with their observed values as truth. Run both methods' predictors on that
synthetic problem and choose the method whose predictor wins. The offline harness of issue 25
showed that the predictors' errors under synthetic clipping (of the darker frame) rank the methods
by scene kind as the real scores do (HUE better on night scenes, OPP on skies), so the self-test
may carry the same signal while using only the clipped frame.

## Steps

1. Offline (Luna, `/tmp/hl-rules-study/tasks/2026-10-24-selftest/task.md`): margins m(f) for
   f in {0.5, 0.7, 0.85} with the predictors of issue 25; selector "gradient domain if m > t";
   scored with the real renders' scores (`pair_align-mgport.json`: opposed against gd-r-mg4), with
   leave-one-scene-out choice of f and t.
2. If the selector captures a clear part of the oracle's gain: implement it in dt-src as a
   TEMPDEBUG mode ("automatic": run the self-test on the RGB grid, then one of the two methods),
   render and score; measure the self-test's run time.

## Done when

The offline selector shows no useful signal (stop), or the prototype is scored on the 103 pairs.

## Result of step 1: the self-test (2026-10-24)

Script by Luna (`/tmp/hl-rules-study/tasks/2026-10-24-selftest/selftest.py`; Luna stopped at the
Codex usage limit before running it; I reviewed it against the task and ran it: `run.log`,
`answer.md`, `selftest.json`, 7.5 min). Per pair, real clipping of the clipped raw's quads at 0.98 T,
synthetic clipping at 0.98 f T, OPP and HUE predictors (issue 25) on the synthetically clipped but
really measured quads; margin = angle(OPP) - angle(HUE). Real winners from `pair_align-mgport.json`
(opposed against gd-r-mg4, pair_table.py's tie rule): 26 gradient domain, 49 opposed, 27 ties, 1 mixed.

- Best in-sample setting (f = 0.5, t = 0.5 degrees): 46 of 75 clear winners right (61%); as a
  selector against inpaint opposed at best 24 / 31 / 47 / 1 (f = 0.5, t = 0): a net loss.
- Leave one scene out (f, t chosen on the other scenes): 8 / 29 / 65 / 1 against inpaint opposed.
- Night scenes: right in 8-9 of 10 clear cases; city and sky scenes: gradient domain picked far too
  often (the predictors on near-clip pixels do not see the occluder colors that hurt gradient
  domain on large regions). **Negative.**

## A one-feature per-image rule (2026-10-24)

`/tmp/hl-rules-study/tasks/2026-10-24-selftest/stump/` (`stump.py`, `stump.log`, `features.json`):
global features of the clipped raw's quads (share of quads with any channel at 0.98 T or more;
median, 10th percentile and mean of max(x / T)); a threshold rule, gradient domain on one side,
inpaint opposed on the other, scored with the real renders' pair labels (so exactly what a
per-image switch in darktable would give).

| feature | leave one scene out, against inpaint opposed (W / L / T / M) |
|---|---|
| clipped share (gradient domain below about 6.6%) | 14 / 7 / 82 / 0 (29 pairs get gradient domain) |
| mean of max(x / T) | 8 / 13 / 82 / 0 |
| median, 10th percentile | 5 / 2-3 / 95-96 / 0 |

Clipped-share rule at fixed thresholds (no fitting), against inpaint opposed and against gradient
domain alone:

| gradient domain if clipped share below | pairs with gradient domain | vs opposed | vs gradient domain |
|---|---|---|---|
| 2% | 16 | 7 / 4 / 92 / 0 | 45 / 19 / 38 / 1 |
| 5% | 21 | 10 / 5 / 88 / 0 | 44 / 16 / 42 / 1 |
| 6.6% | 27 | 14 / 5 / 84 / 0 | 44 / 12 / 46 / 1 |
| 8% | 35 | 16 / 8 / 79 / 0 | 41 / 10 / 51 / 1 |
| 10% | 48 | 21 / 16 / 66 / 0 | 33 / 5 / 64 / 1 |
| always gradient domain | 103 | 26 / 49 / 27 / 1 | - |

From 2% to 8%, the rule beats both methods alone (net +3 to +9 against opposed, +26 to +33 against
gradient domain): it keeps 7 of the 8 night-scene wins and avoids the 31 city and sky losses. It
misses the phone pairs with large clipped areas, where gradient domain wins. The thresholds come
from these 103 pairs (72 scenes, mostly one photographer and two cameras); leave one scene out is
the honest figure.

## Per region against per image, on the renders (2026-10-24)

`/tmp/hl-rules-study/scripts/region_rule.py` (derived from region_oracle.py: same reference,
regions = 8-connected components of the changed mask, composites scored on the whole image like
pair_align.py with PA_HALF=1; it reproduces `pair_align-mgport.json`'s opposed and gd-r-mg4 scores
exactly on g294), runner `runs/run_rule.sh`, log `runs/rule.log`, per pair
`rule-opposed-gd-r-mg4.json`. Clipped share here = share of the render's pixels flagged in
`masks.npz` (any channel), per region or per image. W / L / T / M against inpaint opposed:

| rule | vs opposed | vs gradient domain |
|---|---|---|
| per region, gradient domain below 0.05% / 0.5% / 2% / 5% / 20% of the image | 0/5/98/0, 8/14/81/0, 12/19/72/0, 17/23/63/0, 24/38/41/0 | 47/27/29/0 ... 15/3/85/0 |
| per image, gradient domain below 2% / 4% / 6.6% / 10% / 15% | 3/3/97/0, 8/4/91/0, 12/5/86/0, 19/10/74/0, 21/20/62/0 | 46/23/33/1, 45/18/39/1, 44/14/44/1, 39/7/56/1, 29/5/68/1 |
| leave one scene out, per region | 0 / 23 / 80 / 0 | |
| leave one scene out, per image | 19 / 10 / 74 / 0 | |

- The per-region size rule loses against inpaint opposed at every threshold: small clipped
  regions are not only lamps but also sky gaps between leaves, where gradient domain is worse, and
  composites add seams. Consistent with issue 3: one method per image.
- The per-image rule wins against both methods alone between about 2% and 10% clipped (render
  share) or 2% and 8% (raw-quad share, table above).

**For Kofa:** a per-image switch "gradient domain if under about 7-10% of the image is clipped,
else inpaint opposed" beats both methods alone on these pairs (honest estimate: 14 / 7 or 19 / 10
against opposed, leaving scenes out). Options: (a) an "automatic" mode in the highlights module that
dispatches on the clipped share; (b) gradient domain falling back to inpaint opposed above the
threshold; (c) leave the choice to the user. No new renders are needed to judge it: the renders it
picks are `gd-r-mg4.pfm` / `opposed.pfm` of each pair (list of picks: `rule-*.json`,
`image_rule`). Not done: a darktable prototype (it would only reproduce these scores), a check on
other photographers' images.

Note (2026-10-24): the phone pairs that the per-image rule misses are real gradient domain wins:
re-rendered with the DNG GainMap off (`score-remaining-brackets.md`, "Phone pairs and the DNG
GainMap"), gradient domain still beats inpaint opposed on all 9. Their clipped share is large
(sky), so a rule on the clipped share alone gives them to inpaint opposed.

## Inpaint opposed's own chroma offsets as a second feature (2026-10-24)

`/tmp/hl-rules-study/tasks/2026-10-24-selftest/oppchroma/` (`oppchroma.py`: the global offsets
from the `-d pipe -d verbose` line "opposed chroma" of a small render of each clipped frame, 103
pairs). Leave one scene out, against inpaint opposed:

| rule (gradient domain if ...) | W / L / T / M |
|---|---|
| most negative offset below about -0.135 | 11 / 3 / 89 / 0 |
| largest absolute offset, or offset spread, alone | 0 / 1 and 0 / 5 |
| clipped share below 6.6% (raw quads), for comparison | 14 / 7 / 82 / 0 |
| clipped share below 6.6% or most negative offset below -0.135 | 17 / 7 / 79 / 0 |

A strongly negative offset (on q08: green -0.42) marks some images where opposed's single offset
does not fit, but the combination adds one net pair over the clipped share alone: within the noise
of 72 scenes. Not pursued; the clipped-share rule stays the candidate.

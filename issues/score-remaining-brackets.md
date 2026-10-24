# Score the remaining brackets as real pairs, plus a crop sheet of unrecorded light cores

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: pending (waiting for Kofa's verdict on the crop sheets; redone at reduced resolution 2026-10-24).
Kind: experiment.

## Goal

Extend the real-pair evaluation from the 12 scenes to all of Kofa's brackets, and show Kofa the
light-source cores that no frame records, which no score can judge.

## Context

- Scoring method: `/tmp/hl-article-research-1/HANDOFF.md` section 3.6. Scripts in
  `/tmp/hl-rules-study/scripts/`: `pick_pairs.py` (pairs 0.8-2.3 EV apart), `ev4_run.py` with
  `EV2_KEEP_PFM=1`, `pair_align.py` (align_image_stack, SSIMULACRA 2 on changed pixels and seams,
  linear output at 0 and -1 EV).
- First 12 pairs: `/tmp/hl-rules-study/runs/pairs12-scores.txt`. Gradient domain ("continuous border,
  hue estimate in fully clipped") wins on lamps, loses on skies and clouds.
- Brackets: `/tmp/gradient-domain-eval/bracketed/{city,colored,indoor,night,phone,sky,specular}/`.
  Some use 3 EV steps (g004, g469): no 1-2 EV pair.
- Light cores clipped in every frame (g004 headlights, purple with gradient domain in Kofa's export)
  are excluded from the score; they need a visual check.

## To do

1. Run `pick_pairs.py` on all bracket directories; render inpaint opposed and gradient domain.
2. Score with `pair_align.py` (`PA_HALF=1`); drop pairs with high motion (t63, g347 so far).
3. Crop sheet: per image, the largest clipped regions without a truth, linear at 0 and -2 EV, inpaint
   opposed against gradient domain.
4. Fix first: `pick_pairs.py` names pairs by the clipped frame only, so two pairs with the same
   clipped frame share a workdir (t63). See `tooling-fixes.md`.

## Done when

A table of wins, losses and ties per scene type, and a crop sheet Kofa has judged.

## Result (2026-10-23)

Run: `pick_pairs.py` on all 101 bracket scenes (DNG added; unreadable raws skipped: g289
P1080098.RW2) -> `/tmp/hl-rules-study/plans/allpairs.json`, 141 pairs from 93 scenes; no 1-2 EV pair
for t41 (no clipping), g028, g035, g003, g004, g005, n007, g469. Renders: `runs/run_allpairs.sh`
-> `runs/allpairs/` (the 13 pairs12 workdirs copied in under the new names), no failures.
`pair_align.py` (half resolution) on all; two alignments needed a looser third retry (now in the
script: 40 control points, 5 px). Table: `runs/allpairs-table.txt` (`scripts/pair_table.py`).

Rules of the table: d = SSIMULACRA 2 of gradient domain ("continuous border, hue estimate in fully
clipped") minus inpaint opposed, at 0 and -1 EV; |d| < 2 is a tie; a pair is a win if it wins at one
exposure and does not lose at the other, mixed if it wins at one and loses at the other. Excluded:
motion > 15% (Half Dome, 14%, still agreed with Kofa's verdict) or truth for < 50% of the clipped
pixels: 38 of 141 (most phone pairs, the flower close-ups, t63, t65, n006).

| type | win | loss | tie | mixed | excluded | median d 0 EV | median d -1 EV |
|---|---|---|---|---|---|---|---|
| city | 0 | 29 | 12 | 0 | 7 | -0.5 | -6.2 |
| colored | 3 | 10 | 1 | 0 | 6 | -0.1 | -5.2 |
| indoor | 2 | 2 | 4 | 0 | 8 | 0.0 | -0.3 |
| night | 8 | 0 | 2 | 3 | 4 | +0.4 | +3.8 |
| phone | 8 | 1 | 0 | 0 | 13 | +27.8 | +35.1 |
| sky | 0 | 11 | 1 | 0 | 0 | -3.2 | -24.9 |
| specular | 0 | 3 | 3 | 0 | 0 | -1.2 | -4.2 |
| all | 21 | 56 | 23 | 3 | 38 | -0.1 | -3.1 |

- Confirms the 12-pair result on 103 pairs: gradient domain loses on daylight scenes with clipped
  sky or clouds (city, sky, colored), wins on night scenes and lamps. The large losses are pink or
  magenta overcast skies (t39 piazza lamps -94 at 0 EV, t27 -67, t42 -65, g319, g213), the failure
  of issue 5 (`/tmp/hl-rules-study/scratch/t39-sheet.png`).
- Differences are mostly at -1 EV; at 0 EV most clipped areas are white in both.
- Phone (OnePlus AC2003 DNGs, with GainMap opcodes for lens shading): inpaint opposed overshoots to
  saturated cyan in clipped skies (q08 opposed 1.6 against gradient domain 57.2 at 0 EV,
  `/tmp/hl-rules-study/runs/allpairs/q08-park-blue-sky-45_4-45_2/bl+0_*.png`). With darktable's
  defaults (no sidecar, fresh config) the clipped sky centre is a gray ellipse with a hard edge
  following the vignetting, where the darker frame shows uniform light cyan
  (`/tmp/hl-rules-study/scratch/gainmap/default.jpg`). Whether the gain map causes opposed's
  overshoot is not established; not filed as a darktable bug. Treat phone wins as "inpaint opposed
  fails here", not as a gradient domain strength.

Crop sheets for Kofa (`scripts/crop_sheet.py`: per pair the largest regions clipped in both frames;
inpaint opposed and gradient domain at 0 and -2 EV, truth frame at -2 EV with the unrecorded
pixels flat gray and outlined green):
- `/tmp/hl-rules-study/runs/cores5-sheet.png`: the five light scenes without a 1-2 EV pair (g003 moon,
  g004 and g005 street lamps, g469 sun, n007 stage), middle frame against the darkest
  (`scripts/pick_cores.py`, `plans/cores5.json`, `runs/cores5/`; not scored).
- `/tmp/hl-rules-study/runs/lights-sheet.png`: night and indoor scenes, one pair per scene (the one
  with the darker truth), 2 regions each. Some "night" scenes are daylight (g193, g257); their
  largest unrecorded areas are sky.
- Split into 1200 px high parts: `*-sheet-part<n>.png`.
- First look: at -2 EV inpaint opposed turns unrecorded lamp cores light blue (g004 lamps), gradient
  domain warm white; gradient domain puts a pink patch on the moon (g003).

Removed on 2026-10-23 (cleanup of /tmp): the PFMs and alignments in `runs/pairs12/` (byte-identical copies are in `runs/allpairs/`). Also the alignment inputs in `runs/allpairs/*/align/` and `runs/cores5/*/align/` (`ref.tif`, `tru.tif`,
`mask.tif`, `al_0000.tif`; pair_align.py regenerates them); the aligned truth `al_0001.tif` and the
warped clip mask `mk_*.tif`, which crop_sheet.py, region_oracle.py and border_*.py read, are kept.

2026-10-23 19:20 UTC: the remaining alignment files (`al_0001.tif`, `mk_*.tif`) disappeared from every
`align/` directory under `/tmp/hl-rules-study/runs/` (also `judged-lin/`, `cores5/`), deleted by
something outside this session; renders, masks and scores are intact. Rerun pair_align.py
(`PA_HALF=1`, any `SSIM_METHODS`, a separate `PA_OUT`) to recreate them before using crop_sheet.py,
region_oracle.py, border_*.py or dir_declip.py. Kofa removed them; regenerated the same day with `PA_ALIGN_ONLY=1 pair_align.py`
(146 workdirs: `runs/allpairs/`, `runs/cores5/`; log `runs/realign.log`), which keeps only `al_0001.tif`
and `mk_*.tif`. Results computed on the new alignments match the old ones to six decimals (g294).
`runs/judged-lin/` was not regenerated.

## Reduced resolution (2026-10-24, HANDOFF step 1)

With **full resolution** off, gradient domain ("continuous border, hue estimate in fully clipped",
`gd-r-hold`) against inpaint opposed on the 103 pairs: 26 / 49 / 27 / 1 (win / loss / tie / mixed),
median d -0.1 / -1.3 (`runs/reduced-gd-r-hold-vs-opposed.txt`; full resolution: 21 / 56 / 23 / 3).
Crop sheets at reduced resolution: issue 6's `runs/fills-r-cores5-sheet.png` and
`fills-r-lights-sheet.png` (parts `*-part<n>.png`); their first two method columns (inpaint opposed,
"CB hue fill") are this issue's comparison, now for all 24 night/indoor pairs.

## Phone pairs and the DNG GainMap (2026-10-24)

The phone DNGs (OnePlus AC2003) carry lens-shading GainMaps (gain 1.0 in the centre, up to 4.4-4.7
in the corners, per CFA channel), which rawprepare applies by default ("embedded GainMap") before
highlight reconstruction, while every highlight method keeps one clipping threshold for the whole
frame. On q08's clipped frame, 4.8% of the photosites are clipped and another 14.7% reach the
threshold only because of the gain (raw values down to 0.24 of white); inpaint opposed then turns
that whole sky area saturated cyan, and with the GainMap off only the really clipped centre
(which is still cyan, so opposed has a second problem there). Known darktable issue #12128 (closed
as stale in 2023); our note and a draft comment:
`/tmp/dt-issues/duplicates/12128-highlights-clip-threshold-ignores-dng-gain-map/`. Experiment switch
in dt-src: `DT_NO_GAINMAP=1` (rawprepare, TEMPDEBUG). Consequence for this study: on the phone pairs, the
scores mix this bug with the methods; see the fair comparison below, which shows gradient domain's
phone wins hold without the gain map.

Fair phone comparison done (2026-10-24, `runs/run_phone_nogm.sh`, workdirs `runs/phone-nogm/`,
scores `pair_align-nogm.json`): the 9 included phone pairs re-rendered with the GainMap off for
every frame (truth, clipped, inpaint opposed, multigrid gradient domain at reduced resolution),
re-aligned and scored. Gradient domain against inpaint opposed: 9 / 0 / 0 / 0 (with the GainMap,
`pair_align-mgport.json`: 7 / 2 / 0 / 0). Opposed still fails without the gain map (q08: 25 against
72 at 0 EV, 4 against 54 at -1 EV); its global chroma offsets on q08 are R -0.10, G -0.42, B +0.09
(`-d pipe -d verbose`), so the failure is opposed's single global offset on this sky, not the gain
map. Gradient domain's phone wins are real; the gain map adds a separate, larger failure area.
Also, with the GainMap the motion estimate of q08 was 12.6%, without it 0.0%: the gain map
artifacts disturbed the alignment check.

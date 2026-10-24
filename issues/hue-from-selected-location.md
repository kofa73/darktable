# Hue from one selected smooth, bright location per region

Status: done (2026-10-23): negative; selection is no better than blending. Kind: experiment.

## Goal

Take a region's hue from one good location, as segmentation based does, instead of blending all
border donors, so hair or pole colors are not carried into a flame or a sky.

## Context

- `/tmp/hl-article/HANDOFF.md`, "Hue sources: selection instead of interpolation" (Kofa's view).
- Segmentation based picks one candidate per segment, weighted toward low 5x5 standard deviation
  (`src/iop/hlreconstruct/segbased.c:108-119`).
- Differs from rule F, which failed: F pooled hues per tile across regions (research HANDOFF 3.5).
- Overlaps with `border-classification.md` (which border segments to trust); do that first.

## To do

1. Offline on stage dumps: hue of the best smooth, bright border location against the true interior
   color, versus the blended border hue.

## Done when

Known whether selection predicts the interior color better than blending.

## Result (2026-10-23)

Offline on the 103 pairs of issue 2 instead of stage dumps: `/tmp/hl-rules-study/scripts/border_select.py`,
280 clipped regions of at least 5,000 render pixels (summary `runs/border-select-summary.txt`, per
region `runs/border_select.json`). Border = ring 2 px outside the region, in the clipped frame's own
render. Weights after segmentation based's `_calc_weight()` on luminance: smoothness of the 5x5
window, and brightness. Median u'v' distance to the truth's interior color:

| source | all | city | colored | night | sky |
|---|---|---|---|---|---|
| one location, smoothness only (as segbased.c runs) | 0.0108 | 0.0143 | 0.0129 | 0.0146 | 0.0081 |
| one location, smoothness x brightness^2 (as intended) | 0.0111 | 0.0132 | 0.0151 | 0.0146 | 0.0081 |
| top 2% by smoothness x brightness^2 | 0.0111 | 0.0098 | 0.0169 | 0.0171 | 0.0086 |
| whole border (blend) | 0.0115 | 0.0129 | 0.0177 | 0.0099 | 0.0104 |
| border with >= 0.75 of clip beyond (issue 4) | 0.0049 | 0.0034 | 0.0084 | 0.0037 | 0.0107 |
| inpaint opposed's interior | 0.0043 | 0.0028 | 0.0049 | 0.0133 | 0.0079 |
| gradient domain's interior | 0.0044 | 0.0038 | 0.0030 | 0.0051 | 0.0071 |

- A selected location beats the blend in 46-49% of regions: no better than chance. The top-2%
  variant beats the bright border in only 32% of the 211 regions that have one.
- The bright part of the border predicts about twice as well as any selection or the blend, and
  both methods' interiors already match it, so one selected location has nothing to add.
- In passing: segmentation based's candidate weight ignores brightness (`segbased.c:118`,
  `fmaxf(1.0f, ...)` of a ratio at most 1); already recorded in `/tmp/hl-article/HANDOFF.md:1011`.

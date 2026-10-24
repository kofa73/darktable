# Display clipping in the score and the crop sheets at 0 and -1 EV

Status: pending (2026-10-24): Kofa chose option 1; rescored at -2 / -3 EV, results below. Kind: bug in the evaluation.

## Problem

pair_align.py scores, and the crop sheets show, linear Rec2020 renders converted to 8-bit sRGB by
`lin_eval.to_srgb8()`: the Rec2020 -> sRGB matrix, then each channel clipped to [0, 1]
(`/tmp/hl-rules-study/scripts/lin_eval.py:19-22`), at 0 and -1 EV with the clipped frame's clipping
level at display white. In the clipped areas, the true values (the aligned darker frame) are often
above display white in one or two channels. Clipping those channels separately shifts the hue: a
bright light-blue sky with green and blue over white shows cyan. The methods' renders are clipped
the same way, so the score compares clipped against clipped, and differences in the brightest
channels are flattened.

Found from Kofa's question (2026-10-24) on the reference column of the blend crop sheets ("slightly
cyan-tinted blue sky").

## Measurement

`/tmp/hl-rules-study/tasks/2026-10-24-blend/ref_clip.py`: over the 103 pairs, pixels of the clipped
area (`masks.npz` F) that have a truth (pair_align.py's noTruth excluded), share whose aligned truth
has 1-2 sRGB channels over white (hue distorted) or all 3 (white):

| exposure | 1-2 channels over white: median / mean / max of the pairs | all 3 over white: median |
|---|---|---|
| 0 EV | 34.9% / 38.6% / 97.4% | 52.1% |
| -1 EV | 22.9% / 31.7% / 94.1% | 1.1% |
| -2 EV | 0.1% / 6.8% / 63.6% | 0.0% |
| -3 EV | 0.0% / 0.3% / 12.8% | 0.0% |

Nearly all of it is brightness: the linear Rec2020 values are already over white (at -1 EV, 27.8%
of the pixels on average, against 3.9% that go over only through the sRGB matrix; 0.5% are outside
the sRGB gamut). Worst pairs at -1 EV: q08 (93-94%), t20, g373, g413, t39 (83%).

## Consequences

- At 0 EV about half of the scored clipped pixels are plain white in the truth, and a third more
  are hue-distorted; at -1 EV still a quarter to a third. Every table scored at 0 / -1 EV since the
  switch to SSIMULACRA 2 (issue 21) is affected in these areas.
- The reference column of the blend crop sheets (issue 27) shows hue-shifted truth, not true colors.

## Options

1. Rescore at -2 and -3 EV (pair_align.py has 0 and -1 EV fixed in its loop, `for off in (0, -1)`;
   a small change; about 10 minutes per run of 4 methods). -3 EV leaves 0.3% distorted on average, -2 EV 6.8%.
2. Hue-preserving display clipping (scale a pixel's RGB down by its largest channel when over 1):
   no hue shift, but the brightness of the brightest pixels is lost.
3. A tone mapper: excluded by Kofa before ("no tone mapper, no raised exposure", lin_eval.py header).

## Done when

Kofa has chosen; the key comparisons (inpaint opposed against gradient domain at reduced and full
resolution, the blends of issue 27) are rescored if he wants that.

## Rescore at -2 and -3 EV (2026-10-24, Kofa: option 1)

`pair_align.py` and `pair_table.py` take the exposures from `PA_EVS` / `PT_EVS` now (default `0,-1`,
output unchanged: checked against `runs/blend-blend25-vs-opposed.txt`). Runner `runs/run_ev23.sh`,
scores `pair_align-ev23.json`, tables `runs/ev23-*.txt` (same rules: tie if |d| < 2 at both
exposures; motion and truth-share exclusions as before).

| comparison (103 pairs) | 0 / -1 EV (win / loss / tie / mixed) | -2 / -3 EV | -2 / -3 EV without phone |
|---|---|---|---|
| gradient domain reduced (gd-r-mg4) vs inpaint opposed | 26 / 49 / 27 / 1 | 41 / 39 / 22 / 1 | 33 / 39 |
| gradient domain full (gd-mg6f) vs inpaint opposed | 21 / 56 / 23 / 3 | 42 / 39 / 20 / 2 | 34 / 39 |
| full vs reduced gradient domain | - | 33 / 33 / 36 / 1 | 30 / 28 |
| blend25 vs inpaint opposed | 30 / 13 / 60 / 0 | 52 / 4 / 47 / 0 | 43 / 4 |
| fblend25 vs inpaint opposed | 34 / 12 / 57 / 0 | 62 / 4 / 36 / 1 | 54 / 4 |

By type, gradient domain reduced vs opposed at -2 / -3 EV: city 7/22/12, colored 4/9/1, indoor 2/2/4,
night 11/1/1, phone 8/0/0 (1 mixed), sky 5/4/3, specular 4/1/1. Skies are no longer a clear loss
(5 / 4, at 0 / -1 EV 1 / 9); city scenes still are.

Crop check at -3 EV (`runs/ev3-check-sheet.png`: g315, g213, g057, t39; `blend_sheet.py` with
`BS_EV=-3`): the truth shows the real colors (white clouds, blue sky). Inpaint opposed fills these
skies with a flat beige-gray, which the clipped display at -1 EV hid (the truth was white there
too). Gradient domain is magenta. The blend lies between the two errors, which is why it scores
well; it still shows magenta, the defect Kofa rejected at -1 EV. At -3 EV every method's clipped area
is also much darker than the truth (the real values are several times the clipping level), so these
panels show hue and texture, not brightness.

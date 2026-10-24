# Re-score the existing fills and selectors with the new metric

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: pending (Kofa's look at the crop sheets, redone at reduced resolution 2026-10-24). Kind: experiment.

## Goal

Re-check decisions made on mean dE: the edge fill and the per-region fill selectors (rules A-D).

## Context

- "edge hue to white in fully clipped" (experiments 13, 14) desaturates toward white, which the
  directional article recommends for light sources; it lost to experiment 10 on mean dE only.
- Rules A-D (`/tmp/hl-rules-study/REPORT.md`): ring hue spread (C2, C2r) predicted the better fill
  best; rejected on mean dE.
- g004 headlights turn purple with experiment 10 (Kofa's export); the edge fill may fix that.

## To do

1. Render experiments 10, 11, 13, 14 on the real pairs; score with `pair_align.py`.
2. Crop sheet of light cores for 10 against 13 and 14.
3. If fills differ per region, re-run the C2/C2r selector on the real pairs.

## Done when

Each fill's wins and losses per scene type are known, and Kofa has seen the light cores.

## Notes

- 2026-10-23, GPT-6.1 Sol on the pink fill (`/tmp/hl-rules-study/tasks/2026-10-23-pink-fill/answer.md`,
  key numbers verified): in fully clipped pixels "hue estimate in fully clipped" keeps the ratios
  of the hue estimate `rho` and only scales them up to cover solution and input
  (`_gd_hue_fill()`, gradient.c:768 in dt-src); write-back 4 and the white balance do not change the
  hue. So the fill is exactly as good as `rho`. On g369, "continuous border" fills the overcast sky
  near neutral (median 2.05, 2.09, 2.11, 5% pink); without the continuous border the green-deficient
  border pixels make it pink. On the g294 lamp, the hue fill's fully clipped median (5.38, 3.46, 2.11)
  is close to the truth (5.27, 3.29, 1.98), where inpaint opposed gives (2.23, 1.92, 2.68): a global
  white fill would discard that. Sol ranks the edge fill ("edge hue to white", white where the
  neighborhood is >= 80% fully clipped) as the candidate to test, which is this issue's step 1.

## Result (2026-10-23)

Fills for fully clipped pixels, all with the "continuous border" hue source unless named otherwise,
rendered into the 103 pairs of issue 2 and the 5 light-core scenes (`runs/run_fills.sh`,
`runs/fills.log`), scored together with inpaint opposed in one pair_align.py run
(`pair_align-fills.json`). Tables: `runs/fills-<method>-vs-<method>.txt`. Script names:
hue fill = "continuous border, hue estimate in fully clipped" (`gd-wb4x10`); white fill =
"continuous border, white in fully clipped" (`gd-wb4x11`); edge fill = "continuous border, edge hue to
white in fully clipped" (`gd-wb4x14`); "hue from measured channels, edge hue to white in fully
clipped" (`gd-wb4x13`).

| against the hue fill | win | loss | tie |
|---|---|---|---|
| white fill | 8 | 5 | 90 |
| edge fill | 5 | 24 | 74 |
| measured channels, edge fill | 20 | 49 | 33 |

Against inpaint opposed (win / loss / tie): hue fill 21 / 56 / 23 (3 mixed), white fill 22 / 53 / 25,
edge fill 21 / 58 / 21, measured channels with edge fill 18 / 68 / 17.

- White against hue fill: median difference 0.0 in every scene type. White wins on skies and sun
  (t42 0867 +9.9 at -1 EV, g313 +7.2, t38 0843 +5.6, t27 0764 +5.2, g369 +4.2, phone q03 +15.0, q00 +9.3),
  and loses on colored lights (t51 purple neon shop -32.1 / -21.2, g193 ceiling lamp -5.4, g065 sunset
  -3.6). With the continuous border, the hue fill is already near neutral on overcast skies (g369
  2.05, 2.09, 2.11; t25 2.53, 2.55, 2.99, closest to the truth 2.43, 2.54, 2.93).
- The edge fill loses broadly (t25 0752 -39.7, g369 -22.8, t23 0738 -21.1, t64 -17.5). It takes its
  hue from the neighboring partly clipped pixels, which carry issue 5's pink: fully clipped pink
  share g369 14% (hue fill 5%, white 3%), t25 8.5% (1.8%, 1.9%), with the same median as the white
  fill. On the crop sheet it turns the g004 street lamps and the g003 moon pink-purple at -2 EV.
- Per-region oracle, hue against white fill (`region_oracle.py`, `runs/fills-oracle-summary.txt`):
  at most +0.8 over the better fill of each pair, median 0.0. A per-region fill selector (C2/C2r) has
  nothing to gain on the scored pixels, so step 3 was not run.
- Limitation: the score sees only pixels that the truth frame records. Most fully clipped lamp
  cores are clipped in the truth too and do not count; the crop sheets show them.

Crop sheets for Kofa (inpaint opposed, hue, white and edge fill at 0 and -2 EV; truth frame at -2 EV):
`/tmp/hl-rules-study/runs/fills-cores5-sheet.png` (g003 moon, g004 and g005 street lamps, g469 sun,
n007 stage) and `fills-lights-sheet.png` (14 night and indoor pairs), split into `*-part<n>.png`.
At -2 EV: inpaint opposed makes the g004 lamps light blue, the hue fill warm white, the white fill
white, the edge fill pink-purple. In these renders the hue fill does not make g004's lamps purple;
the purple headlights of Kofa's export must be other lights or another frame (not located).

Recommendation: keep the hue fill. It equals the white fill on skies within the score's resolution
and keeps colored lights (t51, g294); drop the edge fills.

Removed on 2026-10-23 (cleanup of /tmp): renders `gd-wb4x13`, `gd-wb4x14` (crop sheets kept); `gd-wb4x10` and `gd-wb4x11` kept.

## Reduced resolution (2026-10-24, HANDOFF step 1)

The same fills with **full resolution** off: hue fill `gd-r-hold`, white fill `gd-r-wb11`
(`['6', '--wb', '4', '--exp', '11']`), edge fill `gd-r-wb14`; 103 pairs plus the 5 light-core
scenes and the 10 night/indoor sheet pairs outside the 103 (`runs/run_reduced.sh`,
`run_reduced2.sh`), scored in one run with inpaint opposed and issue 7's variants
(`pair_align-reduced.json`), tables `runs/reduced-<B>-vs-<A>.txt`.

| comparison | win | loss | tie | mixed | median d 0 / -1 EV |
|---|---|---|---|---|---|
| white fill against hue fill | 9 | 4 | 90 | 0 | 0.0 / 0.0 |
| edge fill against hue fill | 4 | 21 | 77 | 1 | 0.0 / 0.0 |
| against inpaint opposed: hue fill | 26 | 49 | 27 | 1 | -0.1 / -1.3 |
| against inpaint opposed: white fill | 28 | 46 | 28 | 1 | -0.1 / -1.1 |
| against inpaint opposed: edge fill | 24 | 51 | 26 | 2 | -0.2 / -1.8 |

Same picture as at full resolution: white wins on skies and sun (phone q00 +21.5, q03 +12.2,
g313 +7.0, g216 +6.0, g369 +5.5 at -1 EV), loses on colored lights (t51 purple neon -12.4 / -14.0,
g193 ceiling lamp -7.9, g373 -6.7); the edge fill loses broadly (t03 -31.0 / -21.7, t64 -22.7,
g419 -19.6, g193 -17.0 / -14.0).

Crop sheets for Kofa (inpaint opposed, hue, white, edge fill at 0 and -2 EV; truth frame at -2 EV),
reduced resolution: `/tmp/hl-rules-study/runs/fills-r-cores5-sheet.png` (5 light-core scenes) and
`fills-r-lights-sheet.png` (all 24 night/indoor pairs of `scratch/sheetB.txt`, 2 regions each),
split into `*-part<n>.png` (`scripts/split_sheet.py`). These replace the full-resolution sheets for
the decision; the same sheets serve issue 2. First look at -2 EV: g004 lamps light blue with
inpaint opposed, warm white with the hue fill, white with the white fill, pink-cyan fringed with
the edge fill; g003 moon near neutral with the hue fill, a yellow-pink patch with the edge fill;
g005 "WEEKEND" sign pink with the hue fill where the truth frame shows cyan (opposed light blue,
white fill white); g469 sun orange with the hue fill, white with the white and edge fills.

Recommendation unchanged: keep the hue fill, drop the edge fills.

Removed on 2026-10-24 (cleanup of /tmp): renders `gd-r-wb14` (clearly worse; crop sheets kept); `gd-r-hold` and `gd-r-wb11` kept.

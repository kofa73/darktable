# Holding at full resolution, and the fate of the write-back parameter

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: pending (measured 2026-10-23, redone at reduced resolution 2026-10-24; needs Kofa's decision). Kind: decision.

## Problem

Commit `4acc6630c6` holds pixels a channel solve leaves below their input. It removed the green
specks with **full resolution** off, but made the **write-back** parameter a no-op everywhere
(values 0 and 2 render identically), and some images turn pinker at full resolution
(`fc_Cemetery-Tree-2` sky, a dome, a window).

## Context

- `/tmp/hl-article/HANDOFF.md`, "Findings of 2026-10-13", "Open decision: holding at full
  resolution"; next steps 1-2.
- Kofa's idea: hold only on the reduced grid, keep the original path at full resolution, grey out
  the write-back combobox when full resolution is off. Against it: at full resolution holding
  removed 40% of false green on average, and toggling the checkbox would change the kind of
  artifact.
- Earlier tables (`/tmp/claude-gd-artefacts/ev2/obst1-compare.txt`, `fr-compare.txt`) use mean dE,
  since shown unreliable (research HANDOFF 3.6).
- Related: `hue-estimate-channel-bound.md` may fix the guidance itself and make holding unneeded.

## To do

1. Render the code before `4acc6630c6` at full resolution with write-back "joint, any channel
   clipped, capped", and the current code, on the real pairs; score with HANDOFF 3.6.
2. Show Kofa crops (fire, `fc_Cemetery-Tree-2`, bottles), not only scores.

## Done when

Kofa has decided where holding applies, and whether the write-back parameter stays.

## Measurement (2026-10-23)

Switch `DT_GD_NO_HOLD=1` in `/tmp/hl-rules-study/dt-src` gradient.c restores the path before
`4acc6630c6` (first solution as is, no holding, no clamp); off by default. All variants use
"continuous border, hue estimate in fully clipped", full resolution on unless named. Script names:
`gd-wb4x10` = current code (holding; write-back irrelevant); `gd-nohold` = no holding, write-back
"joint, any channel clipped, capped"; `gd-nohold-wb0` = no holding, write-back "basic"; `gd-r-hold`,
`gd-r-nohold` = full resolution off, write-back "joint, any channel clipped, capped".

1. 103 real pairs (`runs/run_hold.sh`, scores `pair_align-hold.json`, tables `runs/hold-*.txt`),
   wins / losses / ties / mixed:

   | comparison | win | loss | tie | mixed |
   |---|---|---|---|---|
   | no holding, joint capped, against holding | 16 | 12 | 71 | 4 |
   | no holding, basic, against holding | 2 | 17 | 84 | 0 |
   | no holding: joint capped against basic | 21 | 5 | 72 | 5 |

   Without holding, the write-back value matters again, and "joint ... capped" is the better one.
   Against holding, the differences are large in both directions, mostly on skies: g319 +57.8 / +18.4
   and t39 +53.9 / +9.2 (0 / -1 EV), g353 +11.9 / +24.9, g315 +27.0, t12 +20 at -1 EV; g213 -43.2 / -17.2,
   g105 car mirror -21.6, g403 -13.1 at -1 EV, q08 (phone) -17 at 0 EV and +19 at -1 EV. Neither variant
   fixes the skies: without holding g319 is less pink and g213's blue sky turns deep violet
   (`/tmp/hl-rules-study/scratch/hold-sky.png`: truth, inpaint opposed, holding, no holding). The
   cause stays issue 5's; holding only moves which skies fail.
2. `fc_Cemetery-Tree-2` pair (`runs/hold2/`), SSIMULACRA 2 at 0 / -1 EV: inpaint opposed 79.0 / 75.1,
   holding 72.3 / 48.1, no holding joint capped 71.0 / 60.2, no holding basic 72.4 / 47.6. Holding makes
   this sky pinker, as found before.
3. Single frames (`runs/hold2/singles/*-2ev-sheet.png`, -2 EV; top row whole image, then crops):
   - fire (`DSC_0028`): at reduced resolution, no holding brings back green specks at the child's
     head and the flame edge; holding removes them. At full resolution, no visible difference.
   - bar and bottles (`Bar_and_bottles-2ev-sheet.png`): small differences; without holding the lit
     shelf behind the bottles is somewhat yellower, with holding whiter. No truth frame, so no
     verdict; no specks in any variant.
4. Run times (one job, 6 threads, while another ran): fire 20 s with holding, 18 s without at full
   resolution; 8 s and 5 s at reduced resolution.

## Options

1. Keep holding everywhere, drop the write-back parameter (current code; simplest). Scores at full
   resolution are a near tie with the alternative (16 against 12 pairs, large swings both ways).
2. Kofa's idea: hold only at reduced resolution; at full resolution no holding with the write-back
   parameter (default "joint, any channel clipped, capped"). Slightly ahead on the pairs and on
   Cemetery Tree, but two code paths, and toggling **full resolution** changes the kind of artifact.

Neither option fixes the sky tint, which belongs to issue 5. Recommendation: option 1, unless the
crops change the picture; revisit after issue 5, since a fix there may change which path is better.

Removed on 2026-10-23 (cleanup of /tmp): renders `gd-nohold-wb0` (clearly worse); `gd-nohold` kept for the pending decision.

## From the paper (Rouf et al. 2012, read 2026-10-23)

The paper solves each channel as a Poisson problem with Dirichlet boundaries only (eq. 6); it never
holds a clipped channel at or above its input. Holding is this branch's addition.

## Reduced resolution (2026-10-24, HANDOFF step 1)

Same comparison with **full resolution** off, the likely default (issue 15: it scores better and is
4-10x faster). Script names: `gd-r-hold` = current code (holding); `gd-r-nohold` = no holding,
write-back "joint, any channel clipped, capped"; `gd-r-nohold-wb0` = no holding, write-back "basic".
Run `runs/run_reduced.sh` (103 pairs, one pair_align.py run with inpaint opposed and the fills of
issue 6, `pair_align-reduced.json`), tables `runs/reduced-<B>-vs-<A>.txt`.

| comparison | win | loss | tie | mixed | median d 0 / -1 EV |
|---|---|---|---|---|---|
| no holding, joint capped, against holding | 6 | 19 | 74 | 4 | 0.0 / -0.4 |
| no holding, basic, against holding | 1 | 25 | 77 | 0 | -0.1 / -0.5 |
| no holding: joint capped against basic | 10 | 10 | 80 | 3 | 0.0 / +0.1 |
| against inpaint opposed: holding | 26 | 49 | 27 | 1 | -0.1 / -1.3 |
| against inpaint opposed: no holding, joint capped | 23 | 51 | 28 | 1 | -0.2 / -1.8 |
| against inpaint opposed: no holding, basic | 22 | 51 | 28 | 2 | -0.3 / -1.6 |

- At reduced resolution holding is ahead, unlike at full resolution (there 16 / 12 for no
  holding). Large swings remain, mostly skies, in both directions: no holding wins g319 +46.8 at
  -1 EV, g315 +35.9 / +9.9, g353 +28.0 and g413 +26.9 at -1 EV; loses g213 -37.4 / -9.4, phone q08
  -32.5 / -13.4 (both q08 pairs), q07 -21.2 at -1 EV. "Basic" without holding loses to holding on
  25 pairs by small amounts (largest -8.3, g319 at -1 EV).
- `fc_Cemetery-Tree-2` (`runs/hold2/`, SSIMULACRA 2 at 0 / -1 EV, `pair_align-reduced.json`):
  inpaint opposed 79.1 / 75.1, holding 67.0 / 49.6, no holding joint capped 59.8 / 46.9, no holding
  basic 68.0 / 49.9. At reduced resolution holding no longer loses this sky to "joint capped".
- Sheets at -2 EV, reduced resolution (`runs/hold2/singles/*-reduced-2ev-sheet.png`; whole image,
  then the three 256 px windows where the variants differ most): fire, without holding the "joint
  capped" render has a green speck at the child's head (first crop); holding and "basic" do not.
  Bar and bottles: small differences; without holding the lights in the blue column are lighter
  cyan. No truth for either.

Reading: at reduced resolution the data favor option 1 (holding, write-back parameter dropped),
and Kofa's idea (holding only at reduced resolution) amounts to the same thing if **full
resolution** goes away (issue 15). Still Kofa's decision.

Removed on 2026-10-24 (cleanup of /tmp): renders `gd-r-nohold-wb0` (clearly worse; the singles sheets keep its panels); `gd-r-nohold` kept for the pending decision.

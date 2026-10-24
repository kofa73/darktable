# Fix plan: green specks at reduced resolution in gradient domain highlight reconstruction

**Status 2026-10-13: implemented in `4acc6630c6`.** Verification done: build without warnings; the highlights integration tests pass (none of them uses gradient domain); the fire has 0 strongly green pixels; ground truth at full resolution: partly clipped mean ΔE 6.41 -> 6.25, 33 images better and 21 worse by more than 0.5, false green pixels -40%. The fills cannot lose brightness, since they use the larger of solution and input. All write-back values now give the same result. Open: whether to hold pixels at full resolution too, see `/tmp/hl-article/HANDOFF.md`, "Findings of 2026-10-13".

Revision 3, after `fix-plan-discussion-03.md`. Revision 2 called the change an obstacle problem solved
by an active set; it is not that, and the plan now describes it as what it is: a heuristic that pins
undershooting pixels at their input. The loop of up to 3 rounds is replaced by one pinning pass and
a clamp. Revision 1 (relaxing the donor test with the grid spacing) stays withdrawn.

## Problem

With "full resolution" off, the gradient domain method can put saturated green specks along the edge
of a clipped region. Example: `2018-06-23_22-34-52_DSC_0028.NEF`, exported to
`darktable_exported/2018-06-23_22-34-52_DSC_0028_01.jpg`, green spots on the child's head. The edit
uses "continuous border, white in fully clipped", write-back "joint, any channel clipped" and
"as shot to reference". With "full resolution" on, the specks are gone.

## Cause

A channel that is clipped in a photosite was at least as bright as its clipping level, so its
solution should not fall below its input. The channel solves do not enforce that; only the
write-back applies it, per pixel, afterwards. Where a solution falls far below its input, the
write-back turns the error into a color:
- "basic" keeps the channel at its input and lifts the other flagged channels on their own;
- "joint" multiplies all flagged channels by input / solution of the channel that fell furthest
  below. Uncapped, this factor has no limit: at the spot above the head, red is solved to 0.03
  against an input of 1.23, so green is multiplied by about 41.

The fire case, in detail: a strip where only red is clipped lies between the white core and the hair.
Its border pixels on the hair side are not donors (`_gd_continuous()`, `gradient.c:389`), so the
continuous border leaves them out of the channel solves, and nothing holds red up on that side. Red
follows the flame hue (R/B about 3.1) down the steep edge and ends near or below zero.

It happens more often on the reduced grid: the donor test compares neighbors that are 2 photosites
apart there, so fewer border pixels pass (30% against 63% at full resolution on this image), and more
strips have no anchor. The share of red-clipped photosites whose solution is below half the clipping
level is about 9 times higher than at full resolution.

## Proposed change

In each channel solve, hold the pixels that undershoot at their input:
1. Solve the channel as now.
2. Pin every pixel of the channel whose solution is below its input: set it to its input and take it
   out of the unknowns, so it acts as a known boundary value for its neighbors.
3. If any pixel was pinned, solve again.
4. Raise what is still below the input to the input (`fmaxf`); this only removes the solver's
   tolerance residue (below).

This is a heuristic, not the minimum of the Poisson energy under the bound "solution >= input". At
that minimum, a pinned pixel may also lie above its input when its neighbors pull it up; step 2 never
lets it. Pinning is chosen over the exact minimum because the exact minimum brings the green
artifacts back on other images (see "Reason for this plan" and "Rejected approaches").

Why one pass is enough: the second solve sees the pinned pixels at their input, which is above the
values they had in the first solve, with the same guidance. By the discrete maximum principle of the
Laplacian, the remaining pixels can then only rise, so no pixel that was above its input falls below
it. Measured over 111 Bayer raws: the second solve leaves undershoots on 10 images, 1 to 47 pixels,
at most 0.6% of the clipping level; the CG solver's stopping tolerance (`GD_CG_TOL`, relative to the
residual) explains them. Step 4 removes them. On the other 101 images, the second solve leaves none.

Implementation, in the channel loop of `_gd_solve()`:
- keep a copy of the channel's input plane and a mask of pinned pixels (one byte per pixel,
  `dt_alloc_align_*`, freed in `cleanup`);
- index the unknowns with the pinned pixels skipped: `_gd_index_pixels()` already takes a skip mask;
  combine it with `filled` (smooth fill experiment);
- pinned pixels need no change in `_gd_solve_poisson()`: unknowns already read known neighbors from
  `bval` (the RGB plane, which holds the input at pinned pixels), and with the continuous border every
  flagged pixel is in `cknown`.

Documentation: the comment of `_gd_solve()` and the channel solve step in
`dev-doc/iop/highlight/gradient_domain.md`, stating that the result is not the constrained minimum
and why.

## Reason for this plan

- **It addresses the error itself.** The specks come from flagged channels solved below their own
  clipped input, which is physically impossible. Pinning removes that state, whatever caused it: a
  missing anchor, a hue that does not fit the edge, a coarse grid.
- **It does not change which pixels are trusted.** Both donor-test changes fed the hue estimate, the
  hue model and the channel boundaries, and each one regressed other images (the lamp glass; thin
  bright lines). Pinning only acts where a solution is already below its input.
- **It is closer to the truth than the current code, and never adds false color.** Scored against
  ground truth on 179 images (139 real bracketed pairs, the darker frame being the truth, and 40
  synthetically clipped frames), reduced resolution, experiment 11, uncapped "joint, any channel
  clipped". dE is CIE 2000 after the tone map of `ev2.py`; masks as in `ev2.py`. Lower is better.

  | mask | current | pinning | exact minimum (release) |
  |---|---|---|---|
  | partly clipped, mean dE | 6.28 | 5.84 | 5.54 |
  | deeply clipped, mean dE | 15.06 | 14.83 | 14.50 |
  | colored, mean dE | 9.37 | 8.88 | 8.62 |
  | partly clipped, images better / worse than current by > 0.5 dE | | 56 / 15 | 67 / 22 |
  | false green pixels (not green in the truth) | 253,911 | 165,170 | 337,210 |
  | false magenta pixels | 50,471 | 32,534 | 22,390 |
  | images with at least 2x and 100 more false green than current | | 0 | 21 |

  The exact minimum has the lower mean dE, mostly because it is brighter where the truth is brighter
  (deeply clipped, mean log2 luminance ratio to the truth: current -0.72, pinning -0.70, release
  -0.65). But it puts green bands back: on the moon (`syn_g003`), green rises to 249 where the truth
  has 211 (8-bit, tone mapped), over 40,335 pixels. Visible false color is what makes a user reject
  a reconstruction, and the gradient domain method is meant as the alternative when the other
  methods fail, so pinning is the better choice.
- **Earlier counts** (8-bit exports at 2000 px; "strongly green": G - max(R, B) > 100; "magenta":
  min(R, B) - G > 80; "new": no pixel of the same kind within 3 px in the current reduced or
  full-resolution render):

  | image | current | pinning | full resolution |
  |---|---|---|---|
  | fire, strongly green | 617 | 0 | 4 |
  | lamp, strongly green / magenta | 13 / 36 | 4 / 33 | 6 / 19 |
  | bottles, magenta | 45 | 20 | 6 |

  Over 111 Bayer raws: strongly green 55,848 -> 39,796 (full resolution: 43,262); 143 new green and
  785 new magenta pixels in total, scattered single pixels along edges that already have fringes of
  the same kind; no new visible artifact in the crops of the worst images (064, 095, 063).
- **It costs little.** The highlights step took 11% to 34% longer on five test images (fire
  2.0 s -> 2.3 s, P1080140 7.7 s -> 8.6 s); that is at most one extra solve per channel. The exact
  minimum needed 3 to 30 solves per channel and 2 to 5 times the time of pinning on the fire, lamp
  and bottles.

## Rejected approaches

All measured with temporary code, now reverted.
- *Exact minimum under the bound (pinning with release, reviewer's prototype).* Pinned pixels are
  released when the energy decreases by raising them, then the channel is solved again. Lower mean
  dE than pinning, but more false green than the current code on 21 of 179 images (table above), and
  3 to 30 solves per channel. Likely reason (inferred, not verified): the guidance asks for a descent
  below the clipping level that cannot happen; the exact minimum keeps honoring the other gradients
  of that guidance and lifts the surrounding region with it, while pinning keeps the error local.
- *Raise both donor limits to `GD_DONOR_RATIO`^spacing* (revision 1). Fixes the fire (617 -> 9) but
  regresses the lamp glass (13 -> 95 strongly green): the extra donors change the hue model's fit,
  and green-only clipped pixels lose their measured hue.
- *Raise the limits only for the channel-solve anchors, keep the hue donors.* Fire 617 -> 23, lamp
  unchanged, but on the 111 images it adds new colored pixels on 11 of them, visibly on thin bright
  lines: 053 turns from a yellow tint to a green and magenta rainbow, 059 gets new magenta and cyan
  fringes.
- *Drop the boundary mask of the channel solves.* Lamp fine, but 39 new magenta pixels on the
  bottles.
- *Cap or remove the uncapped joint write-back.* It only amplifies the error; the green band stays.
- *Default "full resolution" on.* Hides the bug and costs about 4 times the grid pixels.

## Verification before adoption

1. Build; no warnings.
2. Pinning also changes full-resolution output. Run the 111 images and the ground truth set at full
   resolution, before and after, with the same counts and scores.
3. Fully clipped pixels are also pinned (on the fire, all blue-flagged pixels were); their solution
   is then replaced by the white, hue or edge fill, which reads it. Check that the fills keep their
   brightness.
4. The joint write-back mostly becomes a no-op once no channel is below its input (input / solution
   <= 1). Decide whether the write-back variants are still needed.
5. Highlights integration tests; the embedded fire edit (expect 0 strongly green pixels); timing.

Done since revision 2: the ground truth comparison at reduced resolution (above) and the number of
rounds (one pass, see "Proposed change").

## Not covered or not verified

- X-Trans, linear raws, the darkroom preview.
- Loss of fine detail inside the clipped areas, apart from what the dE and luminance scores show.
- The faint green band on the hair, which remains in all variants and at full resolution.
- The yellow tint of thin bright lines at reduced resolution (053).
- Separate finding, not part of this fix: with OpenMP's default spinning wait policy, the CG solves
  slow down by up to 100 times when other processes keep some CPUs busy (lamp: 42 s; with
  `OMP_WAIT_POLICY=passive`, 0.5 s). Each CG iteration passes several OpenMP barriers.

Evidence:
- ground truth run: `/tmp/claude-gd-artefacts/ev2/obst1/` (per image `scores.json` and tone-mapped
  PNGs), summaries `obst1-compare.txt` and `obst1-colorcount.json` beside it; scripts
  `/tmp/claude-gd-scripts/obst_compare.py`, `obst_colorcount.py`; one image (`syn_g289`) failed on
  a truncated raw file;
- the moon, all methods: `/tmp/claude-gd-artefacts/obst2/moon-all.png`; crops of other images:
  `/tmp/claude-gd-artefacts/obst2/crop-*.png`;
- rounds: `/tmp/claude-gd-artefacts/obst2/rounds/rounds.txt` and `r2/*.log`;
- debug patch (pinning, release, per-round log): `/tmp/claude-gd-artefacts/obst2/pin-release-debug.patch`,
  applied by `/tmp/claude-gd-scripts/patch_obstacle_release.py` (`GD_OBSTACLE=<rounds>`,
  `GD_RELEASE=1`);
- earlier: `/tmp/claude-gd-artefacts/eval-anch/`, `/tmp/claude-gd-artefacts/obst/`,
  `/tmp/claude-gd-artefacts/lamp/`.

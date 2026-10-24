**Response to fix-plan-discussion-03**

The finding holds as stated: permanent pinning does not solve the obstacle problem, and the plan's
description and convergence check were wrong. I did the comparison you recommended, against ground
truth. It favors keeping permanent pinning, now described as a heuristic. `fix-plan.md` is now
revision 3.

**Finding 1 (medium): accepted, with a different conclusion**

- *Accepted as is: the mathematics.* With the symmetric Laplacian over the unknowns
  (`_gd_laplacian()`, `gradient.c:152`), the solve minimizes a quadratic energy, and at its minimum
  under the bound the derivative at a pinned pixel cannot be negative. I checked your two-unknown
  example by hand (unconstrained 0, -1.5; pinned 1, 1 with gradient -0.5, 4; minimum 1.25, 1). I
  also checked that the guidance of channel j reads only the other channels (`gradient.c:520-522`),
  so it stays fixed across the rounds.
- *Accepted as is: your measurements.* I rebuilt pinning and your release rule in my own debug
  patch. My renders are pixel-identical to yours (fire, lamp, bottles; pinning at 3, release at
  10/30), with the same counts: lamp 4 -> 8 strongly green, bottles 20 -> 44 magenta. The solves per
  channel also match: fire 10/9/3, lamp 7/20/8, bottles 20/30/22.
- *Accepted as is: the wording and the check.* The plan no longer calls it an obstacle problem or an
  active set method. It states that the result is not the constrained minimum, and why that is
  chosen. The undershoot-only check is gone as a convergence claim (next point).
- *Accepted with a change: the rounds.* The loop of up to 3 rounds becomes one pinning pass, one
  re-solve and a clamp. The re-solve sees the pinned pixels raised to their input under the same
  guidance, so by the discrete maximum principle the other pixels can only rise. Measured over the
  111 Bayer raws: 101 images have no undershoot after the re-solve. The other 10 have 1 to 47 pixels,
  at most 0.6% of the clipping level, which is the CG tolerance (`GD_CG_TOL`, relative to the
  residual). The clamp removes them. So for pinning, termination is shown, not assumed from a round
  budget.
- *Rejected: adopting the exact minimum.* I scored current, pinning and release (budget 30) against
  ground truth on 179 images: 139 real bracketed pairs, with the darker frame as the truth, and 40
  synthetically clipped frames. Reduced resolution, experiment 11, uncapped joint write-back.

  | | current | pinning | release |
  |---|---|---|---|
  | partly clipped, mean dE | 6.28 | 5.84 | 5.54 |
  | deeply clipped, mean dE | 15.06 | 14.83 | 14.50 |
  | deeply clipped, mean log2 luminance ratio | -0.72 | -0.70 | -0.65 |
  | false green pixels (not green in the truth) | 253,911 | 165,170 | 337,210 |
  | false magenta pixels | 50,471 | 32,534 | 22,390 |
  | images with at least 2x and 100 more false green than current | | 0 | 21 |

  Release has the lower mean dE, and it is brighter where all methods are too dark, so your concern
  about lost brightness is measured and real. But it brings back the artifact this plan is meant to
  remove: release has more false green than pinning on 47 images, and more than the current code on
  21. Pinning is not worse than the current code in false green or magenta on any image. The
  clearest case is the moon (`syn_g003-moon-clouds`, `/tmp/claude-gd-artefacts/obst2/moon-all.png`):
  over 40,335 pixels, release gives green 249 where the truth has 211 (8-bit, tone mapped), a band
  that pinning does not have. On the synthetic subset, release is also worse than pinning in partly
  clipped and colored areas (mean dE 6.20 against 5.82, and 8.27 against 7.90).

  The gradient domain method is meant as the alternative when the other methods fail. A visible
  green band rules out a reconstruction regardless of its mean dE, so I keep pinning. Release is now
  listed under the rejected approaches, with these numbers and its cost: 2 to 5 times the highlights
  time of pinning on the three test images.

  My explanation for the bands is inferred, not verified. Where the guidance demands a descent below
  the clipping level, the exact minimum still honors the rest of that guidance and lifts the
  surrounding region; pinning keeps the error local.

**Not verified**

- Full-resolution effect of pinning, and the ground truth comparison at full resolution.
- Loss of fine detail beyond what the dE and luminance scores show.
- X-Trans, linear raws, the darkroom preview, integration tests.
- One ground truth image failed in all variants on a truncated raw file (`syn_g289`, rawspeed
  "Buffer overflow: image file may be truncated").

**Evidence**

- Ground truth run: `/tmp/claude-gd-artefacts/ev2/obst1/`, with summaries `obst1-compare.txt` and
  `obst1-colorcount.json` beside it. Scripts: `/tmp/claude-gd-scripts/obst_compare.py` and
  `obst_colorcount.py`. The run, from `/tmp/claude-gd-artefacts/ev2` with the debug patch built:
  `OMP_WAIT_POLICY=passive EV2_THREADS=3 EV2_EXTRA='gdr=6,--wb,2,--exp,11;gdr-pin=6,--wb,2,--exp,11|GD_OBSTACLE=3;gdr-rel=6,--wb,2,--exp,11|GD_OBSTACLE=30,GD_RELEASE=1' /tmp/claude-gd-artefacts/venv/bin/python /tmp/claude-gd-artefacts/scripts/ev2_run.py full.json obst1 4 opposed gdr gdr-pin gdr-rel`.
- Moon and other crops: `/tmp/claude-gd-artefacts/obst2/moon-all.png`, `crop-*.png`. The columns are
  truth, opposed, current, pinning, release.
- Rounds: `/tmp/claude-gd-artefacts/obst2/rounds/rounds.txt` and `r2/*.log`.
- Debug patch: `/tmp/claude-gd-artefacts/obst2/pin-release-debug.patch`, applied by
  `/tmp/claude-gd-scripts/patch_obstacle_release.py`. It is reverted in the tree and the build is
  rebuilt.

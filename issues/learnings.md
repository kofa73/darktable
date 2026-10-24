# Learnings of the gradient domain work (2026-10-13 to 2026-10-24)

Kofa's decision of 2026-10-24: **work on gradient domain stops.** The reasons: the method is covered
by Dolby's patent US8861851B2 (issue 17), and on skies it tints near-neutral clouds magenta, which
Kofa finds unacceptable (issues 5, 27). The next direction is **inpaint opposed's brightness in fully
clipped areas** (issue 29). This file keeps what remains valid from the gradient domain work; the
numbered issues hold the details. Reusable code (masked multigrid Poisson solver, pull-push
fill, reduced-grid conversion and write-back, the dome script) is in [salvage/](salvage/README.md);
the evaluation scripts in `tools/`, the agents' notes in `agent-notes/`. The branch itself is
archived locally only, in `/tmp/hl-archive/gradient-hl.bundle`.

## 1. Evaluation: what holds for any method

- **Score.** SSIMULACRA 2 on real bracketed pairs, the darker frame aligned to the clipped one
  (Hugin `align_image_stack`), scored on the changed pixels and their seams (issue 21, script
  `/tmp/hl-rules-study/scripts/pair_align.py`). It replaced mean dE. 103 pairs in
  `/tmp/hl-rules-study/scratch/included.txt`, tables by `pair_table.py` (tie if |d| < 2 at every
  exposure; excluded: motion > 15%, truth for < 50% of the clipped pixels).
- **Display clipping (issue 28).** At 0 and -1 EV the conversion to sRGB clips each channel by itself.
  In the clipped areas a third to half of the truth is then white or hue-shifted, which hid inpaint
  opposed's beige-gray skies at -1 EV. Score at -2 / -3 EV (`PA_EVS=-2,-3`, `PT_EVS=-2,-3`), where
  at most 0.3% (mean) of the truth exceeds white at -3 EV. At -3 EV the clipped areas of every method
  look darker than the truth; the panels then show hue and texture, not brightness.
- **The score is not the verdict.** SSIMULACRA 2 measures closeness to the truth, not plausibility.
  A blend that averaged opposed's beige with gradient domain's magenta scored 52 / 4 against opposed,
  and Kofa rejected it: "Magenta is simply unnatural in the sky" (issue 27). Look at sky crops for
  magenta or cyan before reporting a win.
- **Truth gaps.** Where the darker frame is clipped too, there is no truth; pair_align.py puts each
  method's own pixels into its reference there, so they do not count. Crop sheets must show these
  areas as such (flat gray, `blend_sheet.py`, `crop_sheet.py`), not as a picture.
- **Run-to-run noise.** pair_align.py redoes the alignment on every run; the same render can score
  about 1 apart between runs. Compare methods within one run.
- **Phone DNGs.** Their embedded GainMap is applied before highlight reconstruction and makes
  unclipped photosites look clipped (darktable #12128, note in `/tmp/dt-issues/duplicates/`). Inpaint
  opposed then tints skies cyan, and every competitor "wins" on the 9 phone pairs. Report results
  with and without the phone pairs.
- **Offline harness.** Synthetic clipping of the darker frame in camera space gives an exact truth
  for partly clipped pixels and screens predictors in minutes
  (`/tmp/hl-rules-study/tasks/2026-10-24-lreg-offline/predict.py`). It reproduced the real per-kind
  rankings, but it measures hue only, not fully clipped pixels or visibility; its wins for the blend
  did not hold up visually.
- **Test material gaps** (issue 20): no X-Trans with real clipping, no linear raw.

## 2. The problem itself

- **Inpaint opposed's weakness is brightness, not hue** (Kofa, 2026-10-24; measured in
  `/tmp/hl-rules-study/tasks/2026-10-24-brightness/deficit.py`). In partly clipped pixels its
  brightness matches the truth (median 0.0 EV). In fully clipped pixels it stays near the clipping
  level: median 0.2 EV darker than the truth over 69 pairs, 0.4-0.8 EV on lamps (g294 0.8, g298
  0.6, t60 0.4) and 0.3-0.4 EV on clouds (g213, g315, g057). These figures only cover pixels the
  darker frame records; brighter cores are darker still.
- Gradient domain has no such deficit (0.0 EV): it fills fully clipped areas up to their rim's level.
  Much of its advantage on lamps and night scenes came from that brightness, not from its hue.
- **Hue.** Inpaint opposed's single global chroma offset fails on colored lights; one offset per
  clipped region (issue 9) gains 16 / 6 against plain opposed. On daylight skies its fill is a
  natural-looking beige-gray, visibly closer to acceptable than a magenta tint.
- **Domes.** A core brighter than its rim looks natural on lamps (Kofa prefers the dome versions,
  issue 10). A dome is wrong on uniformly bright surfaces (the moon); no rule yet tells them apart.
  The log dome often reaches its 4 EV cap; the linear one is gentler (t60 x2.8, t57 x1.2). Their crop
  sheets at -2 EV show half of each core clipped by the display (issue 10, 2026-10-24 section).
- **Method choice per image or region.** Per region gave nothing over per image (issue 3); a
  self-test on synthetic re-clipping does not choose well (issue 26); a rule on the image's clipped
  share did (gradient domain below about 4-10%), but it depended on gradient domain.

## 3. Gradient domain (for the record)

- Rouf, Lau, Heidrich 2012; implemented in `src/iop/hlreconstruct/gradient.c` on branch `gradient-hl`,
  documented in `dev-doc/iop/highlight/gradient_domain.md`. The patent's independent claims: hue
  in-painting with a 1-D boundary filter (claim 1; our `_gd_smooth_border()` follows it), hue-guided
  gradient transfer with a weighted channel combination (claim 11), and partly clipped pixels from
  hue ratios times the unclipped channel (claim 20). Read verbatim on 2026-10-24 (issue 17).
- Profile against inpaint opposed: wins on lamps, night scenes, colored lights; loses on daylight
  skies and city scenes, where border pixels of trees and roofs give near-neutral clouds a magenta
  or cyan hue (issues 2, 5). None of the fixes tried removed it: border classification (4), fringe
  exclusion (11), channel bound (8), hue from one location (14), local regression (25), blending
  with opposed (27).
- Reduced resolution ("full resolution" off) scored better than full resolution and was 4-10x
  faster (issues 6, 7, 15).
- **Multigrid solver** (Sol, issue 15, commit `6ea0e47cd6`): multigrid-preconditioned CG; on t25 at full
  resolution 121 s CPU against 728 s for the Jacobi-preconditioned CG. Lessons that carry over to any Poisson or Laplace solve:
  memory-bandwidth bound (2 threads as fast as 12; issue 24); halved aggregation as the coarse
  operator beats rescaling toward Galerkin; float coefficients and compact neighbor storage cut
  10-15% wall time and 16% memory (Sol round 6); byte-identity checks must run single-threaded,
  because OpenMP reductions sum in finish order, and `-ffast-math` lets an exact refactoring change
  the bits; floating components need their gauge fixed.
- Memory: about 190 bytes per unknown worst case for the slimmed solver; darktable does not tile this
  mode (`process_tiling_ready = FALSE`) and only logs when memory is short.

## 4. Negative results (do not repeat without a new reason)

Per-region method choice (3); border classification (4); channel bound on the hue (8); fringe
exclusion (11); directional 1-D declipping as an error predictor (13: weak); hue from one selected
location (14); local regression and the censored Zhang-Brainard estimate (25, offline); self-test
method choice (26); blends of opposed and gradient domain, including adaptive and tint-gated weights
and a blend of the hue inside gradient domain (27); opposed's offset as a smooth Laplace field (27,
offline: 50 / 20 against opposed but loses city scenes; not rendered).

## 5. Working with agents and tools

- Sol (Codex) solved the solver problem in four rounds; verify its timings on an idle machine and its
  claims against the diff. Its detached pipelines keep running after the Codex session times out
  (round 6: `autonomous.py`).
- Gemini's research answers cite real papers but mark its own inferences as "verified"; check claims
  (its "free of the Dolby patent" was wrong).
- Long scoring jobs trip other agents' load gates; run them one at a time.

## 6. /tmp cleanup (2026-10-24, Kofa)

Removed everything generated for the gradient domain work since the branch's fork from upstream
master (`8c64bf7ca9`, 2026-10-09): renders other than the ones below, crop sheets, dumps, logs,
agent task directories, the experiment copy and build (`/tmp/hl-rules-study/dt-src`, `dt-build`,
`asan`), `/tmp/opp-review` (with its worktree of `/workspace/dt-master`), `/tmp/claude-gd-artefacts`
except `scripts` and `venv`, generated folders in `/tmp/gradient-domain-eval`, and the small
gradient domain directories in `/tmp` (hlcolor, hl-review, fill-rule review, paper checks, patent
download, issue #22557 material). About 120 GB freed. Paths in the closed issues that point there
no longer exist.

Kept: all raw files (`/tmp/gradient-domain-eval`, `/tmp/hl`); the scoring tools
(`/tmp/hl-rules-study/scripts`, `/tmp/claude-gd-artefacts/scripts` and `venv`,
`/tmp/ssimulacra2-src`, `/tmp/exiftool-dl`, `/tmp/hlc-eval/cfg`); per pair in
`/tmp/hl-rules-study/runs/allpairs` and `cores5`: `truth`, `none`, `opposed`, `opp-local` renders
and sidecars, `masks.npz`, `scores.json`, `align/`, `pair_align-ev23.json` (scores at -2 / -3 EV);
the offline harness (`tasks/2026-10-24-lreg-offline/predict.py`), the sheet and measurement scripts
(`tasks/2026-10-24-blend/*.py`), the research answers (`tasks/2026-10-24-research*`), and issue 29's
`tasks/2026-10-24-brightness`. Checked after the cleanup: raw counts unchanged, pair_align.py
scores a pair. Not kept: a darktable build for new renders; issue 29 needs a fresh experiment copy
when it gets to darktable renders. The notebooks `/tmp/hl-article` and
`/tmp/hl-article-research-1` were left as they are (background of README.md).

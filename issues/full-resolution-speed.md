# Full resolution under 1 minute of CPU time

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: wip (2026-10-24). Kind: experiment (speed).

## Goal

With the multigrid solver (issue 15, commit `6ea0e47cd6`), reduced resolution meets Kofa's limit
of 1 minute of CPU time per image; "full resolution", which Kofa keeps for now, does not on skies:
t25 28 s wall / 198 s CPU, t39 107 s, g319 114 s, g213 111 s (idle machine, 12 threads, highlights
module). Get full resolution under 60 s CPU on the 10 timing pairs without changing the scores or
raising the memory use, and without slowing reduced resolution.

## Context

- Timing pairs and script: `/tmp/hl-rules-study/scripts/gd_timing.py`, results
  `runs/gd-timing.json` (tag `mgport`).
- Baseline renders: `gd-mg4f.pfm` (full) and `gd-r-mg4.pfm` (reduced) in the 103 workdirs; scores
  `pair_align-mgfull.json`.

## Steps

1. Sol round 5 (`/tmp/hl-rules-study/tasks/2026-10-24-solver-speed5/task.md`): profile, then try a
   better-scaled coarse operator, nested iteration, reuse between solves, less memory traffic.
2. Score Sol's renders (`gd-mg5f`, `gd-r-mg5`) against `gd-mg4f` / `gd-r-mg4` in one run.
3. If it holds: port into the branch with the same single-thread byte checks as issue 15 item 12
   where the numerics are meant to stay, else score.

## Done when

Full resolution under 60 s CPU on all 10 timing pairs with ties on the scores, or a measured
reason why not.

## Result of Sol's round 5 (2026-10-24)

`/tmp/hl-rules-study/tasks/2026-10-24-solver-speed5/` (`answer.md`, `final.json`, source
`gradient-budget1.c`, branch patch `branch-budget1.patch`).

- **Profile, t25 full resolution, 12 threads:** the solver is 98.8% of the module's CPU time (hue
  CG 37%, first channel solves 48%, solves after holding 13%; hierarchy construction 0.5%). The
  rest of `_gd_solve()` takes 1.5 s CPU. So setup reuse cannot help; the V-cycle and CG loops are
  the cost.
- **The solver is limited by memory bandwidth, not by cores** (Ryzen 5 5600X, 6 cores / 12
  threads): 2 threads give the same wall time as 12 at a fifth of the CPU time. t25, full:
  12 threads 26.4 s wall / 201.7 s CPU; solver on 2 threads 26.0 / 43.2; on 1 thread 33.7 / 33.4.
  t06, full: 33.4 / 258.5; 33.8 / 58.0; 41.2 / 42.5. Reduced, t25, all of darktable on 3 threads:
  7.7 / 12.3 against 8.1 / 37.0.
- **Sol's candidate:** the multigrid at full resolution on 1 thread, the rest of the module and
  reduced resolution unchanged. Over all 103 pairs, highlights CPU at most 43.4 s (full) and
  53.6 s (reduced); on the 10 timing pairs at most 35.9 s full (t25), where wall time rises from
  28.4 s to 36.5 s. Same numerics (only the thread count, so the reduction order, changes): Sol's
  own SSIMULACRA 2 run on the 103 pairs gives ties everywhere (largest difference 0.10); renders
  `gd-mg5f.pfm`, `gd-r-mg5.pfm` in the workdirs. Not scored by me with pair_align.py: with
  unchanged numerics it would only measure thread-order noise.
- Not tried by Sol: coarse operator scale, nested iteration, float storage, smoother changes.

**Decision for Kofa:** Kofa's limit is CPU time; the thread cap meets it by not running threads
that only wait for memory. Options: (a) no cap (wall 28 s, CPU 198 s on t25 full); (b) solver on 2
threads (wall unchanged, CPU 43 s; t06 58 s); (c) 1 thread (Sol's candidate: CPU 36 s, wall 36 s);
(d) wait for round 6, which tries to cut the memory traffic itself. My recommendation: (b) as the
default policy if the wall time on 12-thread machines matters, (c) if the CPU figure is what counts;
the right cap depends on the machine's memory channels, so a fixed small cap is a heuristic.

## Sol's round 6, partial (2026-10-24, stopped at 07:41 by the Codex usage limit)

`/tmp/hl-rules-study/tasks/2026-10-24-solver-speed6/` (`task.md`; screening logs `screen.log`,
`next-screen.log`; no answer.md yet). t25 full resolution, solver on 2 threads, wall / CPU, iterations
hue / channels, clipped-mask mean difference from `gd-mg4f`:

| variant | wall / CPU s | iterations | mean diff |
|---|---|---|---|
| baseline | 25.2 / 41.8 | 23,17,26 / 19,47,21 | 1e-6 |
| float coefficients ("coeff") | 23.4 / 39.1 | same | 1e-6 |
| implicit level-0 neighbors | 23.6 / 39.3 | same | 3e-6 |
| fused residual | 24.5 / 41.5 | same | 3e-6 |
| packed storage | 26.9 / 44.4 | same | 1e-6 |
| float coarse vectors | 28.3 / 46.3 | 25,17,28 / 19,50,21 | 7e-4 |
| 1 sweep | 29.3 / 48.6 | 37,25,43 / 28,71,33 | 5e-3 |
| 3 sweeps | 25.6 / 43.0 | 19,13,23 / 16,37,17 | 3e-3 |
| coarse scale 0.25 / 0.33 / 0.4 (toward Galerkin) | 117 / 66 / 97 s wall | up to 467 in green | 8e-3 |

Memory-traffic changes give 6-7% at most; rescaling the coarse operator toward the Galerkin scale
makes it much worse, so the halved aggregation is well chosen. Not screened yet: nested iteration,
float transfers, coarsest level size, rediscretized coarse operator. To resume after the limit
resets.

## Sol's round 6, result (2026-10-24, resumed 10:04; report `answer.md` there)

Sol's Codex session timed out at 11:34; its detached pipeline (`autonomous.py`) finished everything
at about 13:20 (`completion.json`: complete, no errors), including the per-change table at 1 / 2 / 12
solver threads.

Candidate `coeff-neighbors-residual-packed` (source `gradient-coeff-neighbors-residual-packed.c`,
diff `candidate.diff`, 294 lines): float coefficients (diagonal and coarse edge weights, which are
dyadic: integer degrees halved), level-0 neighbors reduced to the two vertical indices plus two bits
in the parity byte for the horizontal ones (the unknowns are in raster order), the residual stored
before restriction, and the four double transfer weights per unknown replaced by a 4-bit source mask
and a 16-row table of the same normalized 9:3:3:1 weights. V-cycle and CG vectors stay double.
Iteration counts are unchanged.

Full resolution, wall / CPU s, solver threads 2 (baseline -> candidate):

| pair | 2 threads | 1 thread | 12 threads | peak RSS MiB |
|---|---|---|---|---|
| t25 | 25.8 / 42.2 -> 22.3 / 36.6 | 34.7 / 34.0 -> 31.6 / 31.7 | 26.4 / 204 -> 20.6 / 150 | 2144 -> 1806 |
| t06 | 33.3 / 56.8 -> 29.5 / 50.3 | 45.0 / 46.0 -> 40.4 / 42.0 | 33.6 / 261 -> 27.0 / 199 | 2281 -> 1914 |
| g213 | 13.9 / 22.9 -> 12.7 / 20.8 | 18.8 / 18.9 -> 16.8 / 17.1 | 14.2 / 105 -> 11.2 / 78 | 1086 -> 928 |
| t39 | 13.8 / 23.4 -> 11.9 / 20.4 | 17.8 / 18.4 -> 16.1 / 17.1 | 13.4 / 101 -> 10.5 / 74 | 1604 -> 1409 |

Reduced resolution t25 / t06 at 2 threads: 7.9 -> 7.2 s and 12.0 -> 11.2 s wall; at 1 thread no
gain. So: 9-14% less wall time at 2 threads, 13-22% at 12, 16% less memory at full resolution.

Other findings (screen table in `answer.md`): float V-cycle vectors, float transfers, a grid-map
stencil, coarsest-level changes, a rediscretized coarse operator and a nested (full-multigrid)
start did not beat the candidate; 3 sweeps per level cut iterations but not time; scaling the
coarse operator toward the Galerkin diagonal is much worse (Sol: the aggregated operator has the
Galerkin operator's long-wavelength scale, 25/16 vs 4 on the diagonal but the same second moment).

Checked by me: Sol's 103-pair comparison (`scores-local.json`): all ties, largest |difference|
0.17 (full) and 0.07 (reduced). Final renders `gd-mg6f.pfm` and `gd-r-mg6.pfm` in the 103 workdirs;
against `gd-mg4f` / `gd-r-mg4` on t25, t60, q07: mean |difference| at most 5e-4, max 0.065 (thread
and rounding noise, as between baseline runs). I read the diff: coherent, but the code needs comments
and a tidy-up before a port (an unexplained `compact` flag, the table without a derivation comment).
Not checked by me: the timings (not re-run; they need an idle machine).

Per-change table, t25 full, wall s at 1 / 2 / 12 solver threads (one run each, about +-1 s noise):
baseline 34.7 / 25.2 / 26.4; float coefficients (with the packed table) 29.7 / 23.4 / 21.9; compact neighbors alone
30.9 / 23.6 / 22.8; the candidate 31.6 / 22.3 / 20.6; nested start 42.3 / 31.1 / 30.2 (worse);
float V-cycle 37.7 / 27.7 / 23.8 (worse). "Float coefficients" (`gradient-coeff.c`) is built on the
packed transfer table, 72 changed lines against the baseline; it gives most of the gain with
identical output on t25, the full candidate the rest within the noise of single runs.

Decision for Kofa: port the candidate, or only the float coefficients with the table (about 10-15% less wall time and 16% less memory, same
output within noise), together with the thread-cap decision of round 5.

Removed (2026-10-24, 13:40): the 34 timing renders (`*.pfm`, 665 MB) and 2 private raw copies in
`/tmp/hl-rules-study/tasks/2026-10-24-solver-speed6/renders/`; their logs and JSON records stay. Kept:
sources, builds, reports, and the final renders `gd-mg6f.pfm`, `gd-r-mg6.pfm` in the 103 workdirs.

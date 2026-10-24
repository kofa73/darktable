# Channel solver: tolerance dependence, early stop, OpenMP wait

Status: done (2026-10-24: Sol's round-4 solver is the branch's solver, commit `6ea0e47cd6`, item 12; full resolution still over the CPU limit on skies, continued in [full-resolution-speed.md](full-resolution-speed.md)). Kind: bug (gradient domain solver).

## Problems

1. **Results depend on the CG tolerance.** On the bottles at reduced resolution, `GD_CG_TOL` 1e-8
   instead of 1e-5 (`src/iop/hlreconstruct/gradient.c:58`) changes the export by 13/255 on average;
   with holding, held green pixels rise from about 42,000 to 154,000. One image measured.
2. **Early stop on very large regions.** `DSC_0535.NEF`, full resolution, one green region of 6 M
   pixels: up to 0.041 times the clipping level off a 1e-9 Python solve (RMSE 0.0025 rendered).
   Inferred cause: CG removes smooth, large-scale error slowly. Not rerun with a tighter tolerance.
3. **OpenMP's spinning wait stalls the CG barriers** under other load: the lamp took 42 s against
   0.5 s with `OMP_WAIT_POLICY=passive`. Could hit users on busy machines.

4. **Run time (Kofa, 2026-10-23): CPU run times over 1 minute are unacceptable**, and would be
   painfully slow even with OpenCL. Free to ignore while experimenting; the final method must meet
   it. Measured on 2026-10-23 (issue 4's run): gradient domain "continuous border, hue estimate in
   fully clipped" at full resolution on daylight city skies (t23, t25, Panasonic, 1600 px export)
   took up to 9 min per darktable-cli export, with 3 threads per job and 4 jobs on 12 cores, so
   inflated by the load; `OMP_WAIT_POLICY=passive` was set, so not the spin wait of item 3.
   Single job, idle machine, 12 threads, passive wait (t25 P1050752, 5488x3664 Bayer, full
   resolution, 5.9 M of 20 M grid pixels clipped, 34,441 border pixels): the highlights module
   took 136 s wall, 1319 s CPU (`-d perf`). CG iterations: hue 1579/1002/1837, channels
   1833/5044/2462; held at input 5.7 M / 41 k / 3.1 M pixels. More than 2x the limit in wall time,
   22x in CPU time. Log: `/tmp/hl-rules-study/scratch/timing/perf.log`.

5. **Timings of 10 pairs (2026-10-23)**, single job, idle machine, 12 threads, passive wait, highlights
   module only, "continuous border, hue estimate in fully clipped", write-back 4
   (`/tmp/hl-rules-study/scripts/gd_timing.py`, `runs/gd-timing.json`):

   | pair (clipped grid pixels at full res) | full res wall / CPU | reduced res wall / CPU |
   |---|---|---|
   | t25 overcast sky (5.9 M) | 154 s / 1066 s | 32 s / 152 s |
   | t39 piazza (3.4 M) | 93 s / 546 s | 9.1 s / 53 s |
   | g213 clouds (2.7 M) | 67 s / 452 s | 10.5 s / 61 s |
   | g369 tree, sky (2.4 M) | 57 s / 405 s | 11.0 s / 65 s |
   | g319 sun, trees (3.3 M) | 56 s / 400 s | 11.0 s / 64 s |
   | g300 station hall (3.6 M) | 36 s / 269 s | 7.5 s / 44 s |
   | q08 phone sky (3.2 M) | 24 s / 171 s | 4.5 s / 26 s |
   | g112 chrome (1.5 M) | 11 s / 69 s | 2.4 s / 13 s |
   | t60 wall lamp (1.1 M) | 6.2 s / 36 s | 1.3 s / 6.5 s |
   | g294 lamp (0.8 M) | 3.3 s / 19 s | 0.8 s / 3.2 s |

   CG iterations per solve: hundreds on lamps, 1000-5000 on skies (green channel of t25 at the cap of
   5000 at full resolution, 6081 counting the second solve at reduced resolution). Reduced
   resolution keeps all ten under a minute of wall time but not of CPU time on the big skies. The
   iteration counts are the target: a multigrid or multigrid-preconditioned solve typically needs
   tens of iterations.

6. **GPT-6.1 Sol's analysis and multigrid prototype (2026-10-23)**, report
   `/tmp/hl-rules-study/tasks/2026-10-23-solver-speed/answer.md` (prototype sources and scripts in the
   same directory). Verified here: the CPU (Ryzen 5 5600X, 6 cores, 12 threads, not 12 cores); the
   prototype's renders of the current code are byte-identical to ours; the scores below.
   - Time on t25 (full resolution): 151 s of 152 s in nine solves: three hue solves (50 s), and per
     channel a first solve plus a second one after holding (red 19 s, green 57 s, blue 25 s); 13,757 CG
     iterations; the masked Laplacian and the vector updates each take about a third. Green's 5044
     iterations are two solves (2623 + 2421), neither at the cap.
   - The current stopping test uses the recursively updated residual; recomputed at exit, the true
     relative residual was 1.9e-5 to 8.1e-5 on the large t25 solves (tolerance 1e-5).
   - Prototype: CG preconditioned with a 2x2-aggregation multigrid (damped Jacobi, bilinear
     transfers, explicit handling of components without anchors). t25: 44 s (double, 1e-5; hue
     23/17/26 iterations, channels 22+5 / 53+38 / 22+12), 90 s (double, 1e-8), 34 s (float, but far from
     the current output); g294 3.2 s (unchanged). Setup is rebuilt for every solve (6-7 s on t25).
   - Scored here (`pair_align-mg.json`, one run, 0 / -1 EV): t25 0752 inpaint opposed 66.4 / 58.3, current
     65.7 / 57.3, multigrid 1e-5 65.5 / 52.4, multigrid 1e-8 65.5 / 52.3; g294 85.2 / 78.7 current,
     85.2 / 78.5 and 85.2 / 78.4 multigrid. The two multigrid tolerances agree with each other and score
     5 lower on t25 at -1 EV: a better-converged solve is not better here. The current solver's early
     stop, or its different treatment of components without anchors, helps this sky. One sky image.
   - Sol's ranking: multigrid inside CG first (5-15x on large skies, few seconds unproven); then a
     reduced-grid solve with only the correction upsampled; hue at lower resolution (at most 1.5x
     overall); coarse-to-fine starts; loop fusion and setup reuse (1.2-2x); dropping the second solve
     after holding (at most 1.23x, quality risk).

## Context

`/tmp/hl-article/HANDOFF.md`, "Solver details found on the way", "The channel solves stop early on
very large regions"; the dev-doc lists the tolerance as a known problem.

## To do

1. Measure 1 and 2 on several images (tighter tolerance, or a multigrid / preconditioned solve).
2. For 3, check whether fewer barriers per iteration or a passive wait policy for the solve helps.

## Done when

Results stable to a tolerance change, and the busy-machine time understood.

## From the paper (Rouf et al. 2012, section 4, read 2026-10-23)

"Our implementation uses a multigrid Poisson solver for all subproblems and takes about one minute
to solve a 10 megapixel image on an Intel Core 2 Duo machine running at 3GHz." Supports multigrid
as the direction (Sol's rank 1), and suggests the 1-minute limit is reachable on a current 6-core
CPU.

## From Krishnan and Szeliski 2011 (read 2026-10-23)

"Multigrid and Multilevel Preconditioners for Computational Photography", ACM TOG 30(6) 177
(SIGGRAPH Asia 2011); `/tmp/hl-article/Multigrid and Multilevel Preconditioners for Computational
Photography.pdf` (ACM version, from Kofa) and the text extract `.txt` next to it. The author copy read
first differed only in the ACM headers and Table 1's CMG row.
- Our solves are homogeneous Poisson problems (unit weights) on masked domains with Dirichlet
  boundaries, their "moderately regular" class. For homogeneous problems (HDR compression, Poisson
  blending), geometric and algebraic multigrid converge fast with a rate independent of the number
  of levels.
- Smoother: "the four-color smoother is a much better choice than Jacobi smoothing for all
  problems, but has an especially dramatic effect for the homogeneous problems". Sol's prototype
  uses damped Jacobi; red-black Gauss-Seidel (the 2-color case for a 5-point stencil) is the next
  thing to try.
- Counting cost per iteration, adaptive hierarchical basis preconditioning with four-color smoothing
  (ABF-Pre7-4C) wins: effective rate 9.52 against 7.69 (AMG-4C) and 4.98 (GMG-J) on a 2048x2048
  Poisson problem (Table 3). Iteration counts alone mislead; memory access matters too (which fits
  Sol's finding that half the time is spent streaming vectors).
- Irregular, elongated structures (thin masks) are where regular coarsening fails; adaptive
  coarsening (AMG, combinatorial multigrid) handles them.

## From Szeliski 2006 (read 2026-10-23)

"Locally Adapted Hierarchical Basis Preconditioning", ACM TOG 25(3) (SIGGRAPH 2006);
`/tmp/hl-article/Locally Adapted Hierarchical Basis Preconditioning.pdf` (from Kofa) and the text
extract `.txt` next to it.
- The preconditioner (LAHBF) builds its interpolants from the system matrix: repeated red-black
  elimination, each eliminated variable interpolated from its 4 neighbors with weights -a_ij/a_ii;
  diagonal links that would widen the coarse stencil are redistributed to the 4 allowed ones
  (eq. 14-16, scale s_N = 2). Bases never cross a tear, and a pixel with a strong data constraint is
  reproduced exactly ("as the data weight w_i -> inf ... the target data value is interpolated
  exactly"). That suits our solves (masked domains, Dirichlet values around them) better than the
  bilinear transfers of Sol's prototype.
- Simplest form for us: the full image grid, known pixels as strong data constraints; no mask
  bookkeeping in the hierarchy, a uniform layout (GPU and OpenCL friendly, as the paper stresses),
  but all 20 M grid pixels instead of the 6 M clipped ones.
- Cost: computing the bases takes a little longer than one preconditioning step; a preconditioned
  iteration costs about 3x a plain CG step; convergence in a handful of iterations on Poisson
  blending, colorization and HDR compression.
- Limit (with Krishnan and Szeliski 2011): regular coarsening degrades on highly irregular, elongated
  domains; clipped sky between branches is such a case, so it must be tested on our masks.
- Candidate for the next prototype, after Sol's round 2 (multigrid-preconditioned CG on all pairs).

## From Kazhdan and Hoppe 2008 (read 2026-10-23)

"Streaming Multigrid for Gradient-Domain Operations on Large Images", ACM TOG 27(3) 21 (SIGGRAPH
2008); `/tmp/hl-article/Streaming Multigrid for Gradient-Domain Operations on Large Images.pdf` (from
Kofa) and the text extract `.txt` next to it.
- Benchmark: on one 2.2 GHz Core 2 core, "a 3-channel, 16-megapixel gradient-domain problem with an
  rms error on the order of 1e-5 in 15 seconds"; 12 MP stitching in 10 s in core (Table 2). Our 20 MP
  solves take 150 s on 6 cores: the solver, not the problem size, is the bottleneck, and the
  1-minute limit is clearly reachable.
- Speed comes from memory locality: several Gauss-Seidel sweeps per pass over the data (temporally
  blocked relaxation), restriction and prolongation pipelined so a V-cycle takes two streaming
  passes. Agrees with Sol's finding that streaming vectors costs half our time.
- Second-order (quadratic B-spline) finite elements make one V-cycle accurate enough for 8-bit output
  (convergence factor about 0.0014 per cycle with 5 sweeps).
- Not directly applicable: full rectangular domain with Neumann boundaries only; Dirichlet values on
  irregular boundaries, our case, is listed as future work ("web-splines"), as is a soft data term
  min |grad u - g|^2 + lambda |u - u0|^2. Using it would mean reformulating our masked solves on the full
  grid with known pixels held by a soft penalty, which makes the problem spatially varying; there
  Szeliski 2006's adapted bases fit better.

## From Farbman, Fattal, Lischinski 2011 (read 2026-10-23)

"Convolution Pyramids", ACM TOG 30(6) 175 (SIGGRAPH Asia 2011); `/tmp/hl-article/Convolution
Pyramids.pdf` (from Kofa) and the text extract `.txt` next to it. Kernels optimized offline (small,
separable, symmetric: 5x5 analysis/synthesis, 3x3 g) approximate large convolutions in O(n) on a
pyramid; the paper's kernels are in its supplementary material (not on this machine).
- Gradient integration: free-space Green's function, 16.8 MP in 0.19 s on one 2011 core (Table 1),
  mean error 0.002-0.005 of the range; no boundary conditions, so not our masked solves as such.
- Membranes over irregular regions (section 5): Shepard interpolation of boundary values with weights
  1/d^3 as a ratio of two convolutions, w * r_hat / w * chi; "clones the same number of pixels [4.2 MP]
  in 0.15 seconds", visually hard to tell from the Laplace membrane. Fits our hue estimate directly:
  the three hue solves are a Laplace membrane of the border colors over U (50 s of 152 s on t25); the
  continuous border is handled by leaving untrusted border pixels out of both convolutions. Exactness
  is not required there, since the hue estimate is a heuristic anyway.
- Idea for the channel solves (ours, not in the paper), from Poisson cloning: where the guidance is
  the gradient of a known image s (here s = r * u_k, the hue-scaled reference), the Dirichlet Poisson
  solution is exactly s + membrane((f - s) on the boundary). So f*_j ~ s_j + Shepard membrane of
  (f_j - s_j) from the boundary, with no iterative solve. It breaks where s jumps (a reference
  becoming unusable, or the weights changing fast); the gradient-domain solve smooths those jumps,
  and Rouf et al. criticize purely spatial methods for such discontinuities. Must be tested on the
  pairs. Holding can stay a max() afterwards.
- Most promising speed idea so far: the hue part certainly fits; the channel part needs a test.
  Prototype offline first (Python on the stage dumps or renders), with kernels fitted as in
  section 3.1 or taken from the paper's supplement if available.

## From Briggs, Henson, McCormick, "A Multigrid Tutorial" (2nd ed., SIAM 2000; skimmed 2026-10-23)

`/tmp/hl-article/Multigrid tutorial.pdf` (from Kofa) and the text extract `.txt` next to it (page
markers `=== page N ===`). Read: chapters 2-4 (relaxation, cycles, cost, FMG), 8 (AMG), parts of 7.
- Cost unit: one work unit (WU) = one relaxation sweep on the finest grid. In 2D a V(1,1) cycle costs
  about 8/3 WU, a full multigrid FMG(1,1) cycle about 7/2 WU (chapter 4).
- Model 2D Poisson problem, red-black Gauss-Seidel, full weighting, linear interpolation: about 0.1
  error reduction per V-cycle (Table 4.2); one FMG(1,1) cycle solves to the level of the
  discretization error on every grid size up to 2048^2 (Table 4.3). Against our 1000-5000 CG
  iterations per solve (each roughly 1-2 WU), that is two to three orders of magnitude fewer sweeps,
  for regular domains.
- Red-black Gauss-Seidel smooths better than weighted Jacobi (chapters 2-3), as Krishnan and Szeliski
  found.
- Irregular or unstructured domains are what algebraic multigrid (chapter 8) is for: it builds
  coarse levels and interpolation from the matrix. On the book's hard example (unstructured mesh,
  jumping coefficients) it reaches 0.31 per V-cycle against 0.1 on the model problem, with an
  operator complexity of about 3 (more setup and memory). Our masks lie on a regular grid but have
  irregular, thin shapes; using multigrid as a CG preconditioner (Sol's prototype, Szeliski 2006)
  compensates for imperfect coarsening.

## Progress 2026-10-23 evening (Kofa away; agents used)

1. Sol round 2 (`/tmp/hl-rules-study/tasks/2026-10-23-solver-speed/answer.md`, "Round 2"): the double
   multigrid-preconditioned CG rendered all 103 pairs at full and reduced resolution, written as
   `gd-mg.pfm` and `gd-r-mg.pfm` in the workdirs; timings in `timings-mg.json`. Highlights module,
   median / maximum wall (CPU): full 11.3 / 57.6 s (67 / 393 s), reduced 2.5 / 15.5 s (13.7 / 95.6 s);
   maxima on t06 0616. All 1,815 solves converged, at most 229 (full) / 288 (reduced) iterations.
   On the 10 timed pairs at reduced resolution: median 8.3 s current against 3.7 s multigrid.
2. Luna (`/tmp/hl-rules-study/tasks/2026-10-23-convpyr/answer.md`, verified by rerunning
   evaluate.py): NumPy convolution pyramid with kernels fitted for Shepard's membrane (F5,3,
   kernels.json). Against the exact Shepard membrane 0.2-1.3% mean of the boundary range (max up to
   17.7% on a real t39 mask); Shepard itself differs from the Laplace membrane by 1.3-3.6% mean.
   0.07 s single-threaded for a 0.7 MP crop, 3 channels.
3. Gemini (`/tmp/hl-rules-study/tasks/2026-10-23-gemini-membrane/answer.md`), checked: rejects the
   membrane replacement of the channel solves (steps where references drop out, no data in fully
   clipped regions, no holding); agreed, dropped. Warns that a Shepard hue estimate has flat spots at
   border points, weights ragged borders by pixel count, and reaches across unclipped obstacles where a
   Laplace solve inside the region does not (true). Its alternative, a smooth pull-push fill without
   the Laplace solves, shares the last two properties; tested too. Its claim that geometric multigrid
   diverges because holding punctures the domain is contradicted by Sol's run.
4. Implemented in dt-src gradient.c (off by default): `DT_GD_HUE_PYR=1` (`_gd_convpyr()`, Luna's
   kernels; P(border colors) / P(border indicator) replaces the pull-push start and the three hue
   Laplace solves), `DT_GD_HUE_PP=1` (bilinear pull-push, Laplace solves skipped). t25 export with
   DT_GD_HUE_PYR: 108 s full resolution (current about 150 s), 19 s reduced.
5. Running: `runs/run_speed.sh` renders gd-r-hold (current code, reduced), gd-pyr / gd-r-pyr,
   gd-pp / gd-r-pp into the 103 pairs and scores them with inpaint opposed, the current code and Sol's
   gd-mg / gd-r-mg in one pair_align.py run (`pair_align-speed.json`).
6. Luna's port (`/tmp/hl-rules-study/tasks/2026-10-23-mg-port/answer.md`; source `gradient.c` there,
   plugin directory `modules/`, build script `build_mg.py`): Sol's double multigrid-preconditioned CG
   behind `DT_GD_MG=1`, combinable with DT_GD_HUE_PYR / DT_GD_HUE_PP; `DT_GD_MG_SMOOTH=rbgs` gives
   symmetric red-black Gauss-Seidel smoothing instead of damped Jacobi. Verified: switches off ->
   byte-identical to the current code on g294 and t25; DT_GD_MG=1 matches Sol's renders up to
   thread-count differences (4 instead of 12 threads; t25 max 0.022). Red-black Gauss-Seidel cuts
   iterations by about a third (t25 green 34+21 against 53+38). Its times were taken on a loaded machine
   (t25 current code 381 s against 150 s idle), so no speed ranking yet; to be timed idle with
   `GT_MODULEDIR=.../modules scripts/gd_timing.py` after the scoring run.
7. **Scores of the speed variants (2026-10-24, `runs/run_speed.sh`, `pair_align-speed.json`, tables
   `runs/speed-*-vs-*.txt`)**, 103 pairs, one scoring run, wins / losses / ties / mixed and median
   difference at 0 / -1 EV against the baseline of the same resolution:

   | variant | against | W / L / T / M | median d |
   |---|---|---|---|
   | current code, reduced resolution | current, full resolution | 48 / 24 / 31 / 0 | +0.0 / +1.4 |
   | multigrid (Sol), full | current, full | 12 / 20 / 71 / 0 | -0.1 / -0.2 |
   | multigrid (Sol), reduced | current, reduced | 7 / 48 / 48 / 0 | -0.7 / -1.6 |
   | hue by convolution pyramid, full | current, full | 17 / 28 / 55 / 3 | 0.0 / 0.0 |
   | hue by convolution pyramid, reduced | current, reduced | 9 / 30 / 63 / 1 | 0.0 / -0.2 |
   | hue by smooth pull-push, full | current, full | 8 / 33 / 62 / 0 | 0.0 / -0.1 |
   | hue by smooth pull-push, reduced | current, reduced | 5 / 29 / 67 / 2 | 0.0 / -0.3 |
   | current code, reduced resolution | inpaint opposed | 26 / 49 / 27 / 1 | -0.1 / -1.3 |

   - Reduced resolution is both 4-10x faster and better on these scores than full resolution
     (at full resolution against inpaint opposed: 21 / 56).
   - A better-converged channel solve (multigrid, true-residual stopping) scores worse, clearly at
     reduced resolution: the current CG's early stop helps. Hypothesis: a solve that stops early stays
     near its start, the per-pixel estimate of paper eq. 4, which on large regions beats the fully
     converged, border-driven solution. Test running: CG capped at 50 / 200 / 1000 iterations at
     reduced resolution (`runs/run_iter.sh`, `pair_align-iter.json`).
   - Both cheap hue estimates lose to the Laplace hue solve; Gemini's warnings (density bias, reach
     across obstacles) hold in the scores. Not pursued further.
   - Removed 2026-10-24 (cleanup): the gd-pyr, gd-r-pyr, gd-pp, gd-r-pp renders (re-render with the switches in dt-src); scores kept.
8. **CG iteration cap at reduced resolution (`runs/run_iter.sh`, `pair_align-iter.json`, tables
   `runs/iter-*.txt`)**, against the uncapped current code: cap 50: 10 / 60 / 30 / 3, median d -0.1 / -4.8;
   cap 200: 5 / 56 / 40 / 2, -0.1 / -2.8; cap 1000: 4 / 23 / 76 / 0, 0.0 / 0.0. The hypothesis of item 7 is
   wrong: stopping early does not help. New hypothesis for the multigrid's losses: the treatment of
   components without Dirichlet anchors (floating, from the continuous border leaving untrusted pixels
   out). CG never changes a solution's component in the null space, so a floating component keeps
   the mean of its start (paper eq. 4 estimate); Sol's multigrid takes the mean of the start for the
   hue but pins one pixel for guided (channel) solves, which can put a whole region at another level.
   Asked Sol (round 3) to keep each floating component's start mean, as CG does, and re-render the
   reduced pairs.
   - Removed 2026-10-24 (cleanup): the gd-r-it50/200/1000 renders; scores kept.
9. **Sol round 3: floating components were the cause (2026-10-24).** Report "Round 3" in
   `/tmp/hl-rules-study/tasks/2026-10-23-solver-speed/answer.md`; solver `mg_solver_round3.inc`, build
   `build_round3.py`, renders `gd-r-mg2.pfm` (reduced resolution, 103 pairs), times `timings-mg2.json`.
   Floating components are common: on g031 at reduced resolution 631 per hue solve (33,284 px) and
   1,254 in the first green solve. Jacobi-preconditioned CG keeps each floating component's
   degree-weighted start mean; the multigrid now does the same (harmonic components analytically,
   guided ones by projecting each preconditioner output to zero mean per component and restoring the
   start mean at exit). Scored here (`pair_align-mg2.json`, tables `runs/mg2-*.txt`):

   | variant | against | W / L / T / M | median d |
   |---|---|---|---|
   | multigrid, CG gauge, reduced | current code, reduced | 7 / 2 / 94 / 0 | 0.0 / 0.0 |
   | multigrid, CG gauge, reduced | inpaint opposed | 26 / 49 / 27 / 1 | -0.1 / -1.4 |
   | current code, reduced | inpaint opposed | 26 / 49 / 27 / 1 | -0.1 / -1.3 |

   Highlights module times over the 103 pairs (single job, 12 threads, nothing else running):
   median 1.9 s, maximum 13.3 s wall (t06 0616), median 14 s / maximum 122 s CPU; at most 222
   iterations per solve. The current code at reduced resolution on the 10 timed pairs: median 8.3 s,
   maximum 31.8 s wall.

**Candidate for the final solver:** reduced resolution (scores better than full resolution and is
4-10x faster) with the multigrid-preconditioned CG and CG's gauge: same quality as the current code,
wall time well under the 1-minute limit on every pair. The CPU time still reaches 122 s on t06 with 12
threads. Not yet ported into the branch: that is a substantial code change (about the size of Sol's
`mg_solver_round3.inc`) and needs Kofa's go-ahead, a review, and an OpenCL plan. Open refinements:
red-black Gauss-Seidel smoothing (a third fewer iterations in Luna's port), setup reuse across
channels, single precision where the residual allows.
   - Removed 2026-10-24 (cleanup): the superseded multigrid renders gd-mg, gd-r-mg, gd-mg-tight (Sol can re-render them with its round-2 scripts); gd-r-mg2 and gd-r-hold kept.
10. **Sol round 4 (2026-10-24): CPU limit met.** Report "Round 4" in the same answer.md; solver
    `mg_solver_round4.inc`, build `build_round4.py`, renders `gd-r-mg3.pfm`, times `timings-mg3.json`.
    Changes: symmetric red-black Gauss-Seidel for channel solves of at least 1,310,720 unknowns (Jacobi
    for smaller ones and for the hue, where red-black Gauss-Seidel moved t25 too much), serial loops
    for domains up to 524,288 unknowns, implicit unit weights and fused passes; double precision kept
    (float variants were faster but less stable). Highlights module over 103 pairs at reduced
    resolution: median 2.5 s wall / 4.6 s CPU, maximum 11.5 s wall / 56.4 s CPU (t06). Closeness to
    round 3: mean difference above 1e-3 on t25 (0.0014) and t27 (0.0022), but round 3 differs from
    itself between repeated runs by a similar amount (OpenMP reduction order, near-threshold holding).
    Scored here (`pair_align-mg3.json`, tables `runs/mg3-*.txt`): against round 3 0 / 0 / 103 ties
    (largest difference 0.1); against the current code 7 / 2 / 94; against inpaint opposed 26 / 49 / 27 / 1,
    as the current code.

**State for Kofa (2026-10-24, superseded by item 11):** a solver exists that keeps the current code's quality at reduced resolution and
stays under 1 minute of CPU time (and 12 s wall) on all 103 pairs. It lives in Sol's task directory as
a replacement highlights plugin, not in the branch. Next: Kofa's decision on porting it (reduced
resolution as the default, Sol's `mg_solver_round4.inc` into gradient.c, a review, an OpenCL plan),
and whether "full resolution" should stay at all, since it scores worse and is 4-10x slower.
   - Removed 2026-10-24 (cleanup): the gd-r-mg2 renders (superseded by gd-r-mg3; same scores).

11. **Port into the experiment copy (2026-10-24, HANDOFF step 2).** Luna
    (`/tmp/hl-rules-study/tasks/2026-10-24-mg-port4/`, task.md, answer.md) put Sol's round-4 solver
    into `/tmp/hl-rules-study/dt-src/src/iop/hlreconstruct/gradient.c` as a TEMPDEBUG block
    (`_gd_mg_*`, lines 392-933): `DT_GD_MG=1` selects it, with the final round-4 configuration
    hard-wired (double precision, bilinear transfers, Jacobi for hue and channel domains below
    1,310,720 unknowns, symmetric red-black Gauss-Seidel above, serial loops up to 524,288 unknowns,
    hue hierarchy reused for the three hue solves, CG's gauge); the `STUDY_*` switches and the code
    of the rejected variants are gone. The old CG is `_gd_legacy_poisson()`, unchanged; it still
    runs when the variable is unset. Combines with `DT_GD_HUE_PYR` / `DT_GD_HUE_PP` (then only the
    channel solves use the multigrid). Copy before the port: `gradient-before-port.c` in the task
    directory. Verified here (cmp): switch off, byte-identical to `gd-r-hold.pfm` (g294, t25, 3
    threads) and `gd-wb4x10.pfm` (g294, full resolution); switch on, g294 byte-identical to
    `gd-r-mg3.pfm`. Luna's further checks (7 pairs, 12 threads): clipped-mask mean difference from
    `gd-r-mg3` at most 1.0e-3 (t25), 2.4e-4 (t27), below 2e-5 elsewhere, within round 4's own
    run-to-run spread; iteration counts equal on 4 pairs, off by 1-2 in one solve on t25, t27, t55.
    Not run: GUI, OpenCL, non-default `DT_GD_CG_TOL` / `DT_GD_CG_MAXITER`.
    - **All 103 pairs (2026-10-24).** dt-build rebuilt with the port; renders `gd-r-mg4.pfm`
      (`DT_GD_MG=1`, reduced resolution, 4 jobs of 3 threads, `runs/run_mgport.sh`), scored in one
      run with inpaint opposed, the current code and Sol's `gd-r-mg3` (`pair_align-mgport.json`,
      tables `runs/mgport-*-vs-*.txt`): against `gd-r-mg3` 0 / 0 / 103 ties, largest difference 0.1;
      against the current code 7 / 2 / 94 (as `gd-r-mg3`); against inpaint opposed 26 / 49 / 27 / 1.
    - **Timing, idle machine, 12 threads** (`scripts/gd_timing.py`, `GT_ENV=DT_GD_MG=1 GT_TAG=mgport`,
      `runs/gd-timing.json`, log `runs/timing-mgport.log`), highlights module wall / CPU on the
      10 pairs of item 5:

      | pair | current, reduced | multigrid, reduced | current, full | multigrid, full |
      |---|---|---|---|---|
      | t25 overcast sky | 31.8 / 152 s | 7.6 / 39.4 s | 154 / 1066 s | 28.4 / 198 s |
      | t39 piazza | 9.1 / 53 s | 2.7 / 12.2 s | 93 / 546 s | 14.1 / 107 s |
      | g319 sun, trees | 11.0 / 64 s | 3.8 / 18.2 s | 56 / 400 s | 14.5 / 114 s |
      | g213 clouds | 10.5 / 61 s | 2.7 / 10.9 s | 67 / 452 s | 14.1 / 111 s |
      | g369 tree, sky | 11.0 / 65 s | 2.7 / 10.8 s | 57 / 405 s | 11.3 / 84 s |
      | g300 station hall | 7.5 / 44 s | 5.3 / 25.5 s | 36 / 269 s | 12.4 / 87 s |
      | q08 phone sky | 4.5 / 26 s | 2.7 / 11.7 s | 24 / 171 s | 11.3 / 85 s |
      | g112 chrome | 2.4 / 13 s | 2.4 / 2.6 s | 11 / 69 s | 7.6 / 35 s |
      | t60 wall lamp | 1.3 / 6.5 s | 0.8 / 0.9 s | 6.2 / 36 s | 3.0 / 11 s |
      | g294 lamp | 0.8 / 3.2 s | 0.6 / 0.7 s | 3.3 / 19 s | 2.3 / 8.4 s |

      Reduced resolution: median 2.7 s wall / 11.3 s CPU (current code 8.3 s / 48 s), maximum
      7.6 s / 39 s, under the 1-minute CPU limit on all ten. Full resolution stays above it on the
      skies (up to 198 s CPU on t25), so with this solver too, the limit holds only with **full
      resolution** off.
    - Removed 2026-10-24 (cleanup): the verification renders in the task directory (PFMs, raw copies,
      per-render configs); logs and result JSONs kept. Kept: `gd-r-mg4` (renders of the ported code)
      and Sol's `gd-r-mg3` (previous session's; now redundant with `gd-r-mg4`).
    - Removed 2026-10-24 on Kofa's go-ahead: Sol's `gd-r-mg3` renders (PFMs and score PNGs, 103 pairs,
      3.0 GB); `gd-r-mg4` replaces them. Kept: `pair_align-mg3.json`, `pair_align-mgport.json` and the
      `mg3-*` / `mgport-*` tables.

12. **Port into the branch (2026-10-24, Kofa: "port sol's solver. for now, keep the full-res
    mode").** Commit `6ea0e47cd6` ("highlights: solve gradient domain Poisson systems by multigrid
    CG"): the multigrid-preconditioned CG replaces the Jacobi CG in
    `src/iop/hlreconstruct/gradient.c` (no switch); `tiling_callback()` reports 241 instead of 71
    bytes per RGB pixel; `dev-doc/iop/highlight/gradient_domain.md` has a new section "The Poisson
    solver".
    - Luna ported and restyled it (`/tmp/hl-rules-study/tasks/2026-10-24-mg-branch/`). I then
      rewrote the block for less memory with the same output: no level-0 edge weights, no parent
      arrays, the restriction gathered through the grid maps (in ascending fine-index order, so the
      sums keep their order) instead of stored 16-entry lists, level 0's right-hand side aliasing
      the CG residual. Peak RSS of darktable-cli, t25 full resolution, 6 threads: old CG 1230 MB,
      ported multigrid 2741 MB, slimmed 2146 MB (about 175 instead of 275 bytes per unknown); CPU
      115 s against 728 s for the old CG (`tasks/2026-10-24-mg-memory/`).
    - **Byte identity needs one thread.** At 3 threads, repeated renders of the same build differ
      (t55: hue iterations 67/54/55, then 69/54/55): libgomp adds the threads' partial sums in
      the order they finish. With `OMP_NUM_THREADS=1 --threads 1`, the branch build and dt-build
      with `DT_GD_MG=1` gave byte-identical PFMs and equal iteration counts on t06, t25, t27, g031,
      g291, t55, g294 at reduced resolution and g294, t25 at full resolution
      (`tasks/2026-10-24-mg-verify/check1.py`, logs `check1-*.log`). Under `-ffast-math`, a
      source-level exact refactoring can still change the bits: an inlined stencil helper in the
      red-black Gauss-Seidel loop did (t06, t25 differed); written out, identical again.
    - Sol's review (`tasks/2026-10-24-mg-review/answer.md`): no defect in the memory reductions,
      OpenMP, failure paths or hue reuse (1,817 synthetic domains, 368 injected allocation
      failures, ASan/UBSan); two comment findings fixed before the commit (190 bytes holds for
      regions that coarsen in both directions, narrow strips need more; the halved aggregation is
      not close to the Galerkin operator of this prolongation).
    - In gradient domain mode the module sets `piece->process_tiling_ready = FALSE`
      (`src/iop/highlights.c:1203`): a memory figure above darktable's budget only logs "processed
      without tiling even if memory requirements are not met" (`src/develop/pixelpipe_hb.c:1792`).
    - **Full resolution scored (`runs/run_mgfull.sh`, renders `gd-mg4f.pfm` by dt-build with
      `DT_GD_MG=1`, `pair_align-mgfull.json`, tables `runs/mgfull-*-vs-*.txt`)**, 103 pairs,
      W / L / T / M: multigrid against old CG, both full: 15 / 3 / 85 / 0; multigrid full against
      inpaint opposed 21 / 56 / 23 / 3 (old CG full: the same); multigrid reduced against
      multigrid full 45 / 23 / 35 / 0.
    - Not run: GUI, OpenCL (none for this method), integration tests (no test uses gradient
      domain; the change only reaches mode 6).
    - Removed 2026-10-24: the verification renders in `tasks/2026-10-24-mg-verify/` and
      `tasks/2026-10-24-mg-branch/`, the raw copies of `tasks/2026-10-24-mg-memory/`, Sol's probe
      binaries; logs, JSON and scripts kept. Kept in the workdirs: `gd-mg4f.pfm` (full resolution,
      baseline for issue 24) and `gd-r-mg4.pfm`.

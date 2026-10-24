# What was kept of branch gradient-hl (2026-10-24)

Kofa deletes the branch `gradient-hl` (local and GitHub). This directory keeps the code and ideas
that are worth reusing and are not, by my reading of the claims, covered by Dolby's patent
US8861851B2 (claims read verbatim, see `../merge-readiness.md`; not a legal assessment). The patent
family is US only (no European or international member), listed as active to 2032-11-30. It is
untracked, like the rest of `issues/`: a `git clean -fdx` in the checkout would delete it.

## Archive

- `/tmp/hl-archive/gradient-hl.bundle` (local only, not in this branch: it contains the patented
  method): the whole branch since its fork from upstream master (`8c64bf7ca9`), 11 commits, last
  `6ea0e47cd6`. Restore with `git fetch /tmp/hl-archive/gradient-hl.bundle gradient-hl:gradient-hl`
  in a checkout that has `8c64bf7ca9`. A record, not a source to merge. Also there: the modified
  `CLAUDE.md` of 2026-10-24 (`CLAUDE.md.diff`, `CLAUDE.md.modified`).
- `gradient_domain.md`: the branch's dev-doc page (design, solver, memory, known problems).

## Extracted code

Copied verbatim from `src/iop/hlreconstruct/gradient.c` at `6ea0e47cd6`; each file's header gives
the line ranges. None compiles alone: `highlights.c` included the file and provided darktable's
headers and macros.

| file | what | depends on |
|---|---|---|
| `masked_poisson_mg.c` | Poisson / Laplace solve on an irregular set of pixels (a mask), Dirichlet values from known neighbors, multigrid-preconditioned CG in double; `_gd_mg_solve()` keeps the hierarchy for several right-hand sides, `_gd_solve_poisson()` is the one-shot form | `_gd_neighbors()`, `_gd_index_pixels()` (included) |
| `pullpush_fill.c` | pull-push pyramid: fills unknown pixels from known ones in O(n); a smooth fill or a start value for the solver | none |
| `reduced_grid.c` | mosaic to an RGB grid of step x step photosites per pixel (Bayer 2, X-Trans 3) with per-channel clip flags (a channel counts as clipped if any photosite of it in the window is); X-Trans plane fit per pattern phase; write-back of a grid result to the photosites by bilinear interpolation, never below the input | `_gd_grid_step()` (in `masked_poisson_mg.c`) |
| `/tmp/hl-archive/fully_clipped_dome.c` (local only) | Rouf 2012 section 3.4: inside a fully clipped region, the log slopes of one channel at the border are spread by a Laplace solve and integrated by a Poisson solve: a smooth bump, Gaussian in linear space | `_gd_solve_poisson()`, `_gd_channel_order()` (not extracted: sorts 3 channels by a count) |

`dome.py` (copied from `/tmp/hl-rules-study/scripts/dome.py`; Python, works on finished linear
renders, not on the branch): the linear and log domes of issue 10. Per fully clipped region, luminance
(linear, or its log) is refilled by a biharmonic extrapolation: the squared Laplacian is minimized
over the region with everything outside fixed, which continues the rim's rising slopes into a dome;
the pixel's RGB is scaled up with its hue kept, at most 4 EV, never darkened. Our own idea, not the
paper's: it came from the directional declipping article's third-difference fit (Selesnick, along
four directions), of which a biharmonic solve is the 2D analog. The paper's section 3.4 fill differs:
Laplace-interpolated border log gradients, then a Poisson solve (quadratic in log); our log dome is
a close relative. It uses no hue estimate, so none of the claims I read applies; issue 29 builds on it.

Patent caveat for `fully_clipped_dome.c` (kept out of this branch for that reason): the method is from the paper the patent is based on, and
the patent's description covers the whole paper. The independent claims I read (1, 11, 20) do not
claim this fill by itself; claim 11 needs a hue-guided gradient estimate with a weighted
combination of channels, which this fill does not use. Ask before shipping it anyway.

## Lessons of the solver (any Poisson / Laplace solve in darktable)

- The solve is memory-bandwidth bound: 2 threads were as fast as 12 (Ryzen 5 5600X). Bytes per
  unknown matter more than flops: float coefficients and two stored neighbors instead of four (the
  unknowns are in raster order) gave 10-15% less wall time and 16% less memory (Sol's round 6, not
  ported: `../full-resolution-speed.md`).
- Coarse operator: halving the aggregated fine graph works; rescaling it toward the Galerkin
  operator's diagonal made convergence much worse (same long-wavelength scale already).
- Floating components (no Dirichlet neighbor) need their gauge fixed: keep the start mean, or set
  it analytically.
- Byte-identity checks must run single-threaded: OpenMP reductions add partial sums in finish order.
  Under `-ffast-math` an exact refactoring (an inlined helper) can change the bits.
- Memory: about 190 bytes per unknown in the worst case; a fully clipped 24 MP image at full
  resolution does not fit an 8 GB machine without tiling, and darktable does not tile a module that
  sets `process_tiling_ready = FALSE` (it only logs the shortfall).
- Timing (t25, 20 MP sky, full resolution): Jacobi-preconditioned CG 728 s CPU, the multigrid 121 s.

## Ideas worth keeping (not extracted as code)

- **Work on a reduced grid, write back to the photosites.** Reduced resolution scored better than
  full resolution and was 4-10x faster for gradient domain; any smooth fill (issue 29's brightness
  lift) can run on the grid.
- **Holding.** Never write a reconstructed photosite below its input value (`_gd_write_back()`): a
  clipped photosite was at least at the clip level.
- **White balance of the fill target.** With white balance "camera reference", equal channels in
  the highlights module are the D65 white, not the scene's: color calibration later adapts from the
  as-shot illuminant, so a fill aimed at equal channels turned fully clipped flames bright blue (B/G
  2.60 instead of 1.03). Commit `20dbfc50fb` aims the fill at the scene illuminant instead
  (`_gd_white()` in the bundle). Any fill that targets "white", such as a brightness lift that sets
  a color, needs the same care.
- **Memory accounting.** `tiling_callback()` reported 241 bytes per RGB pixel; when a module cannot
  tile, an honest figure at least makes darktable's warning meaningful.
- **Evaluation** (pairs, alignment, scores at -2 / -3 EV, crop sheets): `../learnings.md`.

Not kept, because the patent claims them or they only serve gradient domain's hue: the border
filter (`_gd_smooth_border()`, claim 1), hue estimates and ratio fills (claim 20), the guided
gradient field (`_gd_edge_guidance()`, claim 11), the continuity / donor tests of the border, the
model and opposed hue experiments, the joint write-back cap.

## Also in this branch

- `synthetic-clipping-rawprepare.patch`: the TEMPDEBUG hook of `src/iop/rawprepare.c` (`HL_SYNTH_EV=<k>`
  raises export pipes' raw data by k EV and clips it like a sensor), used for the synthetic tests.
  Apply to an experiment copy, never to a commit for upstream.
- `../tools/`: the evaluation scripts (`hl-rules-study/`: pair_align.py, pair_table.py, ssim_eval.py,
  camdata.py, dome.py, ...; `claude-gd-artefacts/`: ev2.py, pfm.py, ...). They expect their old
  places, `/tmp/hl-rules-study/scripts` and `/tmp/claude-gd-artefacts/scripts`, where copies stay.
- `../agent-notes/`: research answers of Sol and Gemini (2026-10-24), the offline harness
  (`predict.py`), the blend and brightness scripts, and the fix-plan review notes of 2026-10-12/13.

Lost in the /tmp cleanup of 2026-10-24: the prototype code of inpaint opposed with local chroma
offsets (`DT_OPP_LOCAL`, issue 9). It lived only in the deleted experiment copy
`/tmp/hl-rules-study/dt-src`; issue 9 describes the algorithm well enough to rewrite it.

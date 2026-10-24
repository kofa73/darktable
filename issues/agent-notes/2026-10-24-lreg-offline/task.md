# Offline test: how well do predictors recover clipped channels? (camera space, synthetic clipping)

## Goal

Research by Sol and Gemini (`/tmp/hl-rules-study/tasks/2026-10-24-research/answer-sol.md`,
`answer-gemini.md`) ranks first: predict a clipped channel from the measured channels of the same
pixel by a **local regression fitted on unclipped pixels nearby**, possibly with a censored
(truncated Gaussian) estimate (Zhang and Brainard 2004). Before anyone writes darktable code, test
in Python whether such predictors recover clipped values better than the predictors behind
inpaint opposed and gradient domain, on the scenes of our bracketed pairs.

Write a script `predict.py` in this directory and run it. Analysis only: do not modify anything
outside `/tmp/hl-rules-study/tasks/2026-10-24-lreg-offline/`.

## Data: synthetic clipping of each pair's darker frame

For each workdir listed in `/tmp/hl-rules-study/scratch/included.txt` (103 lines, `allpairs/<pair>`
relative to `/tmp/hl-rules-study/runs/`), `scores.json` gives `ref` (the darker raw, the truth),
`clipped` (the brighter raw) and `k` (exposure difference in EV between them).

- Load the darker raw as camera-space quads with `quads()` of `/tmp/hl-rules-study/scripts/camdata.py`
  (read it: per 2x2 Bayer quad the mean of each color's photosites, black-subtracted, divided by the
  white level, times the as-shot white balance normalized to green; channel c clips at
  `T[c] = wb[c] / wb[G]`). Use gain `2**k` and no synthetic clip, so the values are the truth at the
  brighter frame's exposure. Skip non-Bayer raws and say how many.
- Exclude quads where the darker frame itself is clipped (any photosite of the quad at or above
  0.98 of white before the gain): no truth there.
- Synthetic clipping: channel c of a quad is clipped where `truth[c] >= 0.98 * T[c]`; its observed
  value is then unknown (at least `0.98 * T[c]`). The rest is observed exactly.
- Evaluate on **partly clipped quads** (1 or 2 channels clipped, at least one measured). Fully
  clipped quads: count them, but do not evaluate.
- Speed: if a pair is too slow, downsample by averaging 2x2 quads before clipping (the same for all
  predictors) and say for which pairs. Use `nice -n 19`, ONE process at a time (another job is
  timing on this machine), and pyamg for the Laplace solves.

## Predictors (each gives, for every clipped channel of a partly clipped quad, a value; then clamp it to at least `0.98 * T[c]`)

1. **OPP**, inpaint opposed (`/workspace/darktable/src/iop/hlreconstruct/opposed.c`, read
   `_calc_refavg()` and lines 320-430): `ref_c = (mean of the cube roots of the other two
   channels)^3`, using their observed values (a clipped other channel enters at its clip level);
   estimate `ref_c + offset_c`, with one global `offset_c` = mean of `(x_c - ref_c)` over quads where
   channel c is unclipped, `x_c` in `(0.2 T[c], 0.98 T[c])`, and within 3 quads of a quad with c
   clipped. (darktable works per photosite with a 3x3 mean; quads are an approximation, fine.)
2. **HUE**, gradient domain's hue estimate (paper Rouf 2012 section 3.2;
   `/workspace/darktable/dev-doc/iop/highlight/gradient_domain.md`): border B = unclipped quads
   4-adjacent to the clipped set U (U = quads with any channel clipped, including fully clipped);
   hue `rho = rgb / (r+g+b)` on B (no bilateral smoothing needed), interpolated over U by a Laplace
   solve with B as Dirichlet values (per channel of rho). Estimate for clipped c: the mean over
   measured channels s of `x_s * rho_c / rho_s`.
3. **LREG(R, p)**, local weighted linear regression: for clipped channel c with measured set S,
   `x_c = a + sum_{s in S} b_s x_s`, fitted per quad by weighted least squares over the "training"
   quads (all three channels unclipped, and not dark: max channel above 0.2 of its T) in the
   (2R+1)^2 window around it, weight `(max_j x_j / T[j])^p` (emphasis on samples near clipping).
   Use box filters of the weighted moments (guided-filter style), with a small ridge term on the
   slopes (say 1e-4 times the weighted variance of x_s). Where the window has too little weight
   (choose a threshold and report it), fill the coefficients (a, b) from where they are valid by a
   Laplace solve over the invalid area (coefficient transport). R in {8, 32, 128} quads, p in {0, 4}.
   Fit the two-survivor model where S has 2 channels and the one-survivor model where S has 1.
4. **LREG-T**: as LREG(32, 4), but replace the clamp by the censored estimate: with residual
   standard deviation `sigma` from the same weighted fit (window), `mu` the regression prediction,
   estimate `mu + sigma * lambda(alpha)`, `alpha = (0.98 T[c] - mu) / sigma`, `lambda` the inverse
   Mills ratio `phi(alpha) / (1 - Phi(alpha))` (Zhang and Brainard 2004, JOSA A 21(12)).
5. **GREG**: one global weighted regression per (c, S) over all training quads within 3 quads of a
   clipped quad, weight as LREG with p = 4 (opposed with fitted slopes).

## Errors

Per pair and predictor, over the evaluated quads and clipped channels: mean and median of
`|log2(estimate / truth)|` of the clipped channel; and the chromaticity error of the whole quad
(estimate in the clipped channels, observed elsewhere, against the truth): angle between the two RGB
vectors in degrees, mean and median. Also the share of the evaluated quads per pair.

## Report (`answer.md` here, plus `predict.json` with per-pair numbers)

- A table per scene kind (the folder after `/bracketed/` in `ref`: sky, night, phone, specular, ...)
  and overall: median over pairs of each predictor's mean errors, and per predictor the number of
  pairs where it beats OPP and where it beats HUE (lower mean angle; count ties within 0.1 degree).
- The same split by 1 versus 2 clipped channels.
- Runtime per predictor (single-threaded Python, so only relative).
- Which pairs you skipped or downsampled, and plainly what you did not verify.

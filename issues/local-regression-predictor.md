# Predict clipped channels by local regression on the measured channels

Status: done (2026-10-24): negative offline; local regression loses to the opposed-like predictor everywhere. Kind: experiment (research idea).

## Goal

Find a predictor for partly clipped pixels that beats inpaint opposed on skies and keeps gradient
domain's gains on lights. Candidate from the research of 2026-10-24
(`/tmp/hl-rules-study/tasks/2026-10-24-research/`: Sol's `answer-sol.md` and Gemini's
`answer-gemini.md`, prompt `prompt.md`): fit `missing = a + sum b_s * measured_s` by weighted least
squares on unclipped pixels nearby, weighting samples near clipping, and transport the coefficients
into large regions; optionally the censored (truncated Gaussian) estimate of Zhang and Brainard,
"Estimation of saturated pixel values in digital color imaging", JOSA A 21(12), 2004.

How it differs from what was tried: inpaint opposed is this model with all slopes fixed (mean of the
opposed channels in cube-root space) and one global offset; its per-region offsets (issue 9) keep
the slopes fixed. Gradient domain's hue is a ratio from the border pixels' colors, interpolated by
a Laplace solve, and so takes the colors of occluders on the border. A window regression weighted
toward bright samples fits the relation between channels instead of carrying border colors.

## Research results (2026-10-24)

- Both agents rank this family first (Sol: local regression plus censored estimate, then
  coefficient transport as in Ansel's 2026 "harmonic transposition", then joint sparse declipping
  after Mansour et al. ICASSP 2010, then a small learned model; Gemini: Zhang-Brainard with
  luminance-gated border covariance, then color lines after Elboher and Werman 2010/2011, then a
  guided filter, then edge-stopped multiscale propagation after RawTherapee's `HLRecovery_inpaint`,
  then a CNN).
- Not verified by me: run times, the content of papers other than the patent claim list, Gemini's
  dataset figures (it gives 1,500 scenes for RawHDR, Sol 324 captures; they conflict).
- Patent: see `merge-readiness.md`, inputs of 2026-10-24. A hue-ratio fill of partly clipped
  pixels is claimed by US8861851B2 by itself (claims 20, 42); an affine regression is a different
  formulation, but whether that matters is a legal question.

## Steps

1. Offline test in camera space with synthetic clipping of the darker frames (exact truth, no
   alignment): Luna, `/tmp/hl-rules-study/tasks/2026-10-24-lreg-offline/task.md`. Predictors: an
   opposed-like global offset, a gradient-domain-like Laplace hue ratio, local regressions with
   several windows and weights, the censored variant, a global regression.
2. If a regression clearly beats both on skies without losing on lights: implement it in dt-src as a
   TEMPDEBUG experiment (partly clipped pixels from the regression, fully clipped ones unchanged),
   render the 103 pairs and score with pair_align.py against inpaint opposed and gradient domain.

## Done when

The offline test shows no gain (then stop), or the darktable prototype is scored on the 103 pairs.

## Result of step 1 (2026-10-24)

Luna, `/tmp/hl-rules-study/tasks/2026-10-24-lreg-offline/` (`predict.py`, `answer.md`, per-pair
numbers in `predict.json`). The darker frame of each of the 103 pairs, scaled by `2^k` to the
brighter frame's exposure, clipped synthetically at 0.98 of each channel's level in camera space
(2x2 quad means, as-shot WB; 88 large pairs averaged once more over 2x2 quads); evaluated on the
11.2 M partly clipped quads. Predictors: OPP (inpaint opposed's cube-root reference plus one global
offset per channel), HUE (gradient domain's Laplace interpolation of the border chromaticity, then
ratios), LREG(R, p) (local weighted affine regression in a (2R+1)^2 window, weight
`(max/T)^p`, coefficients transported by a Laplace fill where a window has too little support),
LREG-T (censored estimate, Zhang-Brainard), GREG (one global regression).

Per-pair mean RGB angle error, wins / ties (0.1 degree) / losses against OPP, all 103 pairs:

| predictor | vs OPP | vs HUE | median of the mean angle |
|---|---|---|---|
| OPP | - | 50 / 11 / 42 | 1.50 deg |
| HUE | 42 / 11 / 50 | - | 1.69 deg |
| LREG(128, 0), best regression | 33 / 10 / 60 | 27 / 17 / 59 | 1.81 deg |
| LREG(32, 4) | 22 / 9 / 72 | 10 / 12 / 81 | 2.73 deg |
| LREG-T | 26 / 9 / 68 | 13 / 13 / 77 | 2.63 deg |
| GREG | 3 / 10 / 90 | 13 / 6 / 84 | 3.28 deg |

- The harness reproduces the real ranking by scene kind: HUE beats OPP on night scenes (12 / 1 / 0)
  and loses on skies (2 / 2 / 8) and city scenes (10 / 3 / 28), as gradient domain does against
  inpaint opposed on the real pairs. It is a cheap screening tool for predictors (25 min for all
  predictors on 103 pairs, one core).
- Local regressions are worst with two clipped channels (one measured channel: median angle
  5.1-7.4 against 3.7 for OPP), where the affine fit extrapolates from darker samples; weighting
  toward bright samples (p = 4) makes every variant worse; larger windows are better, i.e. the
  closer to a global fit, the better, but the global affine fit (GREG) is worst of all. With one
  clipped channel, LREG(128, 0) is close to OPP (31 / 33 / 39) and beats HUE (58 / 24 / 21).
- I read the fit and transport code (`_fit_local()`, `local_regression()`): weighted moments by box
  filters, intercept from the weighted means, Laplace transport; no defect found.
- Conclusion: no darktable prototype. The slope-1, cube-root-space reference of inpaint opposed is
  a stronger prior than a fitted affine relation.

/boost
# Research: when to trust a highlight reconstruction, per region (confidence for fusing two methods)

Do not modify any file except /tmp/hl-rules-study/tasks/2026-10-24-research2/answer-gemini.md, which
you write. Do not run or stop any process other than your own short read-only commands. No downloads
of datasets.

## Context

darktable's highlights module reconstructs clipped raw photosites. Two methods:
- inpaint opposed: a clipped channel = mean of the other two channels in cube-root space, plus one
  global chroma offset per channel learned from unclipped photosites near clipping. Robust on skies;
  wrong on colored lights and on fully clipped cores (stays near the clip level).
- gradient domain (Rouf et al. 2012, "Gradient domain color restoration of clipped highlights"):
  a hue estimate interpolated from border pixels by a Laplace solve, then Poisson solves guided by
  the measured channels' gradients. Wins on colored lights and night scenes; on daylight skies it
  takes the hue of dark occluders (trees, roofs) at the border and tints near-neutral clouds
  magenta or cyan.

Result found today on 103 real bracketed pairs (truth from the darker frame, SSIMULACRA 2 at 0 and
-1 EV): the per-photosite geometric blend `opposed^(1-t) * gradient^t` with a FIXED t beats
inpaint opposed: t = 0.15: 23 wins / 5 losses; 0.25: 30 / 13; 0.35: 35 / 16; 0.5: 32 / 24
(gradient domain alone: 26 / 49). An offline camera-space test of the partly clipped pixels
agrees (blend 0.25 wins 67 / 6). A per-pixel adaptive weight that lowers t where the two estimates
disagree (t0 * exp(-|log2(gd/opp)| / s)) did NOT beat the fixed weight. A per-image rule on the
clipped share of the image (more gradient domain on images with little clipping) adds 3-4 wins.
The losses are skies and city scenes where gradient domain's hue is wrong (occluders at the border,
sun glare).

Already tried and negative: local affine regression of the clipped channel on the measured ones
(Zhang-Brainard censored variant included), classifying border segments as sky or occluder,
fringe exclusion, hue from one selected location, four-direction 1D declipping, a self-test that
clips the image again synthetically to choose the method per image.

## Question

Find published methods or well-founded measures that predict, per connected clipped region (or per
pixel), whether a hue/chroma estimate for clipped highlights is reliable, so that the blend weight t
can be raised where gradient domain is right and lowered where it is wrong. Examples of the kind of
thing wanted (verify, do not assume): confidence or uncertainty maps in inpainting or declipping;
consistency of border chromaticity (variance, bimodality) as a reliability measure; agreement of
several estimators; region shape and size features; exposure-fusion style weights (Mertens et al.)
adapted to method fusion; learned fusion weights that need no training set or only a tiny one.

For each candidate: full citation with a link you actually opened, the measure in one or two
formulas, why it would separate "occluder-tinted sky" from "colored lamp", the CPU cost on a 24 MP
raw (target: well under 1 s for the measure itself), and whether it needs training data. Rank the
candidates; say plainly which claims you verified in the source and which you infer. Keep it under
1500 words. Write the answer to /tmp/hl-rules-study/tasks/2026-10-24-research2/answer-gemini.md.

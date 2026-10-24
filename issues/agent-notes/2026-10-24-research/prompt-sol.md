# Research: new ideas for clipped highlight reconstruction in darktable

Research only: do not modify any file except your answer file named below. No dataset downloads;
web search and reading papers online are fine. Work for at most about one hour.

## Setting

darktable's **highlight reconstruction** module rebuilds clipped camera channels in raw data
(Bayer and X-Trans, before demosaicing; linear raws too). Its methods: **inpaint opposed** (fills a
clipped channel from the mean of the unclipped "opposed" channels plus one global chroma offset per
channel), **segmentation based** (per clipped segment, takes the color of a selected location on its
border), and an experimental **gradient domain** method following Rouf, Lau, Heidrich, "Gradient
Domain Color Restoration of Clipped Highlights" (CVPR workshops 2012): hue estimate by Laplace
interpolation of the border colors, then a Poisson solve per clipped channel guided by the
gradients of the measured channels, multigrid-preconditioned CG. It is covered by US8861851B2
(Dolby, active to 2032), which is a problem for merging it.

Scoring: 141 bracketed real raw pairs (a clipped frame and a 1-2 EV darker frame as truth, aligned),
103 usable; SSIMULACRA 2 of each render against the darker frame where any method changed pixels.

What we know:
- Gradient domain beats inpaint opposed on compact colored light sources (lamps, neon, candles,
  colored objects) and keeps lightness detail in deep clipped areas; it loses on extended
  near-neutral areas: daylight skies and clouds next to trees, roofs, lamp posts, where the border
  hue estimate takes the colors of those objects (pink or cyan skies). Rouf 2012 states this limit
  itself (section 3, Figure 6). Overall: gradient domain 26 wins, 49 losses, 27 ties against
  inpaint opposed.
- Fully clipped regions (all three channels): any method can only guess; flat cores of lamps look
  wrong.

Already tried, do not propose again unless you have a materially different variant (say what is
different): per-region choice between inpaint opposed and gradient domain (no gain); sky versus
occluder classification of border segments (look outward along the normal; negative); hue from one
selected smooth bright location instead of a blend (negative); four-direction 1D declipping and its
disagreement as error predictor (negative); clipped channel's lower bound inside the hue estimate
(negative); keeping chromatic-aberration fringe pixels out of the color sources (negative); inpaint
opposed with per-region chroma offsets (modest gain, 16 wins / 6 losses); a dome (brighter core) for
fully clipped lights (pending visual verdict); white / edge-to-white fills for fully clipped pixels;
fill selectors from boundary chroma dispersion; automatic slider settings for segmentation based
(small gain); smooth pull-push or convolution-pyramid hue estimates (worse than the Laplace hue).
Parked by the maintainer, do not propose: Masood et al. 2009 edge-aware hue estimate, and fitting
color against log brightness to extrapolate a sky's color trend.

Constraints for a usable method: CPU only acceptable (OpenCL optional), under 1 minute of CPU time
per 20-24 MP image on a desktop, memory a few GB at most, no external model downloads at run time
(darktable does have an optional AI subsystem with ONNX models, so a small learned model is not
excluded, but say what training data it would need), works on raw mosaic data or on a half-size RGB
grid made from it.

## Questions

1. What published methods (papers, theses, patents, open-source code such as RawTherapee, ART,
   dcraw, LibRaw, Adobe/Lightroom descriptions, smartphone pipelines) reconstruct clipped
   highlights in raw or linear data, beyond the three above? For each: core idea, what failure
   case it addresses, cost, data needed, and patent status if you can find it. Prefer methods that
   handle extended near-neutral regions (skies) well and methods that avoid the Rouf/Dolby claims.
2. Which of them, or which new combination, is most likely to beat inpaint opposed on skies while
   keeping gradient domain's gains on lights? Rank at most five candidates.
3. For each candidate: a concrete experiment in our setting (what to implement, on which of the
   pairs, what result would count as success or failure), and its expected effort.

Give references with authors, title, year and venue or URL. Mark clearly which claims you checked
in the source and which you inferred. Be concise; a wrong citation is worse than none.

Write your answer to /tmp/hl-rules-study/tasks/2026-10-24-research/answer-sol.md.

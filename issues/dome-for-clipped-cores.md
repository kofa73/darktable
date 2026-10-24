# A brighter core than its rim for fully clipped lights

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: pending (prototype and crop sheets done 2026-10-23, redone at reduced resolution 2026-10-24; Kofa's verdict). Kind: experiment.

## Goal

Let fully clipped light sources rise above their surroundings instead of staying flat.

## Context

- In gradient domain, fully clipped regions are a Laplace solve
  (`dev-doc/iop/highlight/gradient_domain.md:177`): by the maximum principle the core can never be
  brighter than its rim.
- The directional article proposes a third-difference fit (Selesnick-LS / RBAR) along four directions,
  continuing the rising slopes into a dome; a biharmonic solve is the 2D analog (HANDOFF 3.7 item 4).
- Cores of lamps and the sun usually have no truth; judge visually.

## To do

1. Offline prototype on dumps or renders: biharmonic or D3 fill of the core luminance, hue unchanged.
2. Crop sheets on lamps and the sun at 0 and -2 EV.

## Done when

Kofa has judged whether domes look better than flat cores.

## Prototype (2026-10-23)

`/tmp/hl-rules-study/scripts/dome.py`, offline on the linear renders of "continuous border, hue
estimate in fully clipped" (`gd-wb4x10.pfm`). Per fully clipped region (all three channels with at
least half of their photosites clipped, 8-connected, at most 50,000 render pixels; larger ones,
skies, are left alone): the squared Laplacian of the luminance is minimized over the region with
everything outside fixed (biharmonic extrapolation on a crop with a margin of the region's own size),
which continues rising slopes into a dome. Where the dome is brighter, the pixel's RGB is scaled up
(hue kept), at most 4 EV; nothing is darkened. Two variants: on log luminance (`gd-dome`) and on
linear luminance (`gd-domelin`). About 1 s per image in Python.

- Score: no change at all (g294, t60 0034: identical to the hue fill at 0 and -1 EV,
  `pair_align-dome.json`). The raised pixels are cores the truth frame does not record either, so
  the score cannot see them; only a visual check can judge.
- Size: the log dome often hits the 4 EV cap (t60 0034: median raised pixel x16); the truth of t60
  where it records the core suggests about x1.7 over gradient domain (fully clipped median truth
  5.68, 3.03, 7.48 against 3.43, 3.42, 4.18). The linear dome raises t60 by x2.8, t57 x1.2, t64 x1.15.
- Crop sheets (inpaint opposed, hue fill, log dome, linear dome at -2 and -4 EV; truth frame at -2 EV):
  `/tmp/hl-rules-study/runs/dome-cores5-sheet.png` and `dome-lights-sheet.png`, split into
  `*-part<n>.png`. First look: at -4 EV the log dome gives the g004 street lamps brighter centers, the
  linear dome a subtler gradient; on the moon (g003, a flat disk in reality) the dome brightens the
  center, which is wrong. A dome suits light sources with a falloff, not uniformly bright surfaces;
  no rule here tells them apart.

Removed on 2026-10-23 (cleanup of /tmp): renders `gd-dome`, `gd-domelin` (regenerate in seconds with `scripts/dome.py`; crop sheets kept).

## From the paper (Rouf et al. 2012, section 3.4, read 2026-10-23)

The paper already fills fully clipped regions with a dome: Laplace interpolation of the boundary's
log gradients, then a Poisson solve in log space, "quadratic over the clipped region. In linear
space, this corresponds to a Gaussian extrapolation". Only applied when one channel's clipped region
equals the fully clipped one (Ω∩ = Ωk). Implemented as the experiment value "smooth fill (paper
3.4)"; `dome.py`'s log variant is a close relative (biharmonic in log luminance) with the same risk
of overshoot.

## Reduced resolution (2026-10-24, HANDOFF step 1)

`scripts/dome.py` with `DOME_SRC=gd-r-hold` (log: `DOME_OUT=gd-r-dome`; linear:
`DOME_OUT=gd-r-domelin DOME_LINEAR=1`) on the 5 light-core scenes and the 24 night/indoor pairs
(`runs/dome-r.log`). Crop sheets (inpaint opposed, hue fill, log dome, linear dome at -2 and -4 EV;
truth frame at -2 EV): `/tmp/hl-rules-study/runs/dome-r-cores5-sheet.png` and
`dome-r-lights-sheet.png`, split into `*-part<n>.png`. Not scored (the raised cores have no truth).

Removed on 2026-10-24 (cleanup of /tmp): renders `gd-r-dome`, `gd-r-domelin` (regenerate in seconds with `scripts/dome.py`; crop sheets kept).

## Display clipping in the crop sheets (2026-10-24, issue score-display-clipping)

The sheets convert with `lin_eval.to_srgb8()`, which clips each sRGB channel at white by itself.
Regenerated `gd-r-dome`, `gd-r-domelin` on the 29 pairs of `runs/dome-r.log` (list in
`/tmp/hl-rules-study/tasks/2026-10-24-blend/dome-dirs.txt`) and measured with `dome_clip.py` there:
share of the fully clipped pixels (28 pairs that have some, pixel-weighted) shown over white.

| method | -2 EV: 1-2 channels over (hue shifted) | -2 EV: all 3 (white) | -4 EV: 1-2 channels | -4 EV: all 3 |
|---|---|---|---|---|
| inpaint opposed | 16.7% | 0.0% | 0.0% | 0.0% |
| hue fill (gd-r-hold) | 36.0% | 0.2% | 0.6% | 0.0% |
| log dome | 25.5% | 32.3% | 4.7% | 15.9% |
| linear dome | 29.5% | 20.0% | 3.7% | 1.3% |

At -2 EV about half of each dome's core is shown clipped: a white center, often with a hue-shifted
ring where one or two channels clip. At -4 EV the linear dome is almost unclipped, the log dome
still a fifth (it often reaches its 4 EV cap). Part of the domes' natural look at -2 EV is this
clipping. In real use a tone mapper compresses these values instead of clipping each channel. Renders
`gd-r-dome.pfm`, `gd-r-domelin.pfm` are kept in the 29 workdirs for a redraw.

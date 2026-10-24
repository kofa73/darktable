# Inpaint opposed: brightness of fully clipped areas

Status: wip (2026-10-24). Kind: experiment. Kofa's direction after stopping gradient domain.

## Goal

Make inpaint opposed's fully clipped areas as bright as the scene was, keeping its hue (which Kofa
finds right) and its partly clipped pixels (whose brightness already matches the truth).

## Context

- Kofa (2026-10-24): "The problem with opposed is not the hue, it's the brightness." He prefers the
  dome versions of lamps (issue 10): "lamps are supposed to be bright".
- Measured (`/tmp/hl-rules-study/tasks/2026-10-24-brightness/deficit.py`, `deficit.json`; pixels
  the darker frame records, linear Rec2020 luminance, median per pair of log2(truth / method)):

  | | partly clipped | fully clipped |
  |---|---|---|
  | inpaint opposed | 0.0 EV (103 pairs) | 0.2 EV darker (69 pairs; quartiles 0.0 / 0.4) |
  | gradient domain (gd-r-mg4) | 0.0 EV | 0.0 EV |

  Opposed, fully clipped: g294 0.8, g065 0.6, g298 0.6, g369 0.6, t60 0.4, g213 0.4, g315 0.4,
  g057 0.3 EV. Censored: the brightest cores are clipped in the darker frame too and are not counted.
- Opposed builds a fully clipped pixel from its other channels, clipped too, plus the chroma offset,
  so it stays near the clipping level (issue 9).
- Patent (issue 17): the claims of US8861851B2 concern hue in-painting with a boundary filter,
  hue-guided gradient transfer and hue-ratio fills. A brightness-only fill is a different formulation;
  whether that matters is a legal question.

## Steps

1. Offline prototype on the existing `opposed.pfm` renders, fully clipped regions only, opposed's
   chromaticity kept:
   a. flat lift: luminance filled from the rim by a Laplace solve, raised where it exceeds opposed's;
   b. dome: `scripts/dome.py` with `DOME_SRC=opposed`, linear and log (copy in `salvage/dome.py`).
2. Score against plain opposed at -2 / -3 EV (issue 28) and measure the remaining deficit with
   `deficit.py`.
3. Crop sheets with hue-preserving display clipping (scale an over-white pixel down as a whole), so
   that brightness, not per-channel clipping, is judged: lamps, the sun, clouds, the moon.
4. If Kofa likes it: implement in `src/iop/hlreconstruct/opposed.c` behind a switch in a new
   experiment copy (the old one, `/tmp/hl-rules-study/dt-src`, was removed on 2026-10-24; build from
   upstream master), then score the real renders.

## Done when

Kofa has judged the crop sheets, and the chosen variant is scored on the 103 pairs.

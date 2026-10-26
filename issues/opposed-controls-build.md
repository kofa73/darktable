# Experiment build: GUI controls for the inpaint opposed variants

Status: wip (2026-10-25; 8c0538ed4a committed; adding per-region chroma after Kofa's test). Kind: experiment build, requested by Kofa.

## Goal

A darktable build, committed on branch `opposed-controls` (from `hl-research`), in which the
variants of issues 29 and 30 can be tried in the GUI: highlight reconstruction, method inpaint
opposed, section "opposed experiments". With every new control at its default the output is
inpaint opposed's. Kofa builds and tests it himself.

## Controls

Estimate of partly clipped pixels (issue 30, ported from the experiment copy's environment
switches): reference passes, iterate where (warm lights / all partly clipped), cube-root offset.

Brightness fill of fully clipped regions (issue 29, ported from `tools/opposed-brightness/lift2.py`):
brightness fill (off, flat lift, Poisson dome, distance dome, biharmonic dome), solve on (linear /
log luminance), max rise, cap (soft / hard), rim occluder threshold, merge clipped specks up to,
round the dome ridge, fully clipped threshold, fill partly clipped too, dome strength (new: scales
the measured rim slope).

## Differences from the prototype (known in advance)

- The fill runs inside highlight reconstruction on raw data (cells: 2x2 Bayer quads, 3x3 X-Trans
  blocks), not on the finished Rec2020 render; luminance from white-balanced camera RGB with
  Rec2020 weights.
- New CPU code only: when a new control is off its default, the module falls back to the CPU.
- Module params version 5 -> 6: edits made with this build do not load in other builds.

## Steps

1. Luna implements (task: `/tmp/hl-rules-study/tasks/2026-10-25-controls/task-luna.md`).
2. Sol reviews; Luna fixes.
3. I build (OpenCL on and off), render with darktable-cli: defaults against opposed (identical),
   rim fix against `opp-rg10`, distance dome against `rg-hdomelin` (close, not identical), timing.
4. Commit on `opposed-controls`.

## Done means

The branch builds, defaults reproduce opposed, the variants reproduce the offline results
approximately, and the commit is in place for Kofa.

## Progress (2026-10-25)

- Luna implemented (task-luna.md); Sol's review found 8 problems: crop width taken from the cell
  size (heap overflow on g298, no region solved on t60 / g294), cube-root chroma cached across a
  switch to segmentation based, solver stopping early (biharmonic 1.01 instead of 2), unit mix in
  rings and Poisson source, 9x9 blur for the ridge, percent sliders stuck at 0 / 1, 8-connected
  exterior for specks, OpenCL veto in other modes.
- Luna's fix round left a diverging multigrid (NaN residual on every real region, minutes per
  region on g294); its probes had constant / planar boundaries that the plane seed solves exactly.
- Sol rewrote the solver: double CG with a Galerkin multigrid preconditioner, true-residual check
  (1e-8), unchanged-and-logged on failure. 50 scipy comparisons, max error 3.8e-6. Real renders:
  all regions solved; distance dome fill 0.1-0.5 s per image, biharmonic up to 2.7 s.
- My checks (`/tmp/hl-controls/test/`, render.py, cmp.py, cmpchange.py, deficit_test.py):
  defaults bit-identical to the unmodified build (RelWithDebInfo) on 6 pairs; reference passes
  (warm lights, 10) reproduce `opp-rg10` exactly on t60, g294, g298.
- Fill against the prototype, median deficit of fully clipped px in EV (opposed / prototype / C;
  with the rim fix: prototype / C): g298 0.63 / 0.62 / 0.63; 0.29 / 0.29. g294 0.79 / 0.77 /
  0.76; 0.57 / 0.45. g065 0.64 / 0.04 / -0.08; -0.09 / -0.39. g213 0.39 / 0.11 / 0.29; 0.11 /
  0.29. Lamps match or improve; the sunset overshoots more, the clouds are lifted much less.
- I simplified the OpenCL fallback (only reference passes, cube-root offset and fill matter) and
  committed (8c0538ed4a). Builds with OpenCL on and off.
- Luna compared the C fill with lift2.py on identical input (the 1600 px render as cells;
  `/tmp/hl-controls/equiv/report.md`): same selection, regions, rings, slopes, limits and caps;
  gains differ by > 0.05 EV on 0.5% (g213), 1.8% (g065), 0.001% (g294) of pixels, from the blur
  (darktable's recursive Gaussian vs OpenCV's kernel, about 6% of the large regions' pixels) and
  from other selected regions: the prototype lets their dark cells be free, the C code keeps
  them fixed as my task asked (up to 0.2 EV in a few small regions). No port bug found.
- So the real-pair gap (g213 +0.18 EV, g065 -0.12 EV against the prototype) comes from working on
  raw cells: about four times as many small regions, and different rim values and slopes in
  camera RGB (on a raw proxy g213's main region gets no positive slope, hence almost no dome).

## Kofa's test and per-region chroma (2026-10-25)

Kofa: most controls hardly do anything (exports `/tmp/gradient-domain-eval/iain/darktable_exported/
Laneway bracket set clipped*.jpg`: defaults, cube-root offset, cube-root + flat lift + fill partly
clipped; `opposed-vs-gd.png` there: gradient domain gives the lamp area a bright, natural look).

- My renders from the export's edit (`/tmp/hl-controls/laneway/`, `crops.jpg`, `zoom.jpg`): every
  variant (rim fix, dome, both, strength 3 / max rise 4) leaves the lamp core light blue and the
  brick strip and plaster below it nearly unchanged; gradient domain makes the core warm white and
  the strip a lighter cream rising toward the lamp. The difference is mostly hue; the dome and rim
  fix keep opposed's hue, the cube-root offset turns it pink, and flat lift cannot rise above its
  rim by construction.
- Cause of the blue core, estimated from the raw with a quad-level reimplementation: opposed's
  image-wide offsets R +0.54, G 0.00, B +0.01; around the lamp and the wall below it R +0.74 to
  +0.87, B -0.53 to -0.55 (cool lights on the right pull the image-wide blue offset up).
- Kofa agreed to a "chroma offset: image-wide / per region" control (issue 9's opp-local design,
  rewritten): task `/tmp/hl-rules-study/tasks/2026-10-25-controls/task-luna-local.md`.
- Luna implemented it; checks: image-wide pixel-identical (v6 sidecar through the 6 -> 7 step);
  per region matches the old `opp-local` renders on 7 of 8 pairs (median difference <= 0.014 EV;
  t60 0034's old render differs from opposed by a uniform 0.02 EV, not a chroma effect); no sky
  tint (g065's lilac with the dome is opposed's own sky hue, the same with image-wide offsets).
  Laneway (`/tmp/hl-controls/laneway/zoom-local.jpg`): plaster and bricks under the lamp lighter
  and more neutral; the core lavender instead of light blue, not warm.
- Sol's review: out-of-bounds lookup on incomplete edge cells, float sums (0.15 EV on a 60 MP
  region), memory estimate; Luna fixed them; I replaced its sum/offset union by plain fields.
  Committed as f1a6903bfb.
- Why the core stays lavender: in white-balanced data the channels clip at different levels
  (this Nikon R 1.49, G 1, B 2.16; `opposed.c` clips = clipval * coeffs), and a clipped photosite
  becomes max(value, estimate), so blue cannot drop below its clip level whatever the offset.
  Proposed next control, waiting for Kofa: "fully clipped color" (opposed / white / from rim).
- Codex budget ran low on 2026-10-25 around 09:50, back at about 11:35.

## Fully clipped color (2026-10-25, my own implementation; c7aac62995)

- Control "fully clipped color": opposed / white / from rim, on the fully clipped cells (with
  merged specks) of the fill's selection; runs before the dome. A cell gets the smallest
  brightness at which the new color keeps every channel >= its current value, at most "max rise"
  above its largest channel. From rim: chroma (channels / largest) of the bright cells around the
  region (3x3 mean), carried inward by the Laplace solve; dark rim cells (rim occluder threshold)
  are free.
- First "white" used opposed's D65 correction: the Laneway core came out sky blue (sRGB 177, 214,
  244). Neutral in highlight reconstruction's own (as-shot) space renders neutral (208, 208, 208),
  so the color step uses no correction.
- First "from rim" put yellow-green and pink bands on g213's sky (leaves on the rim); freeing dark
  rim cells removed them, a faint warm gray remains where the truth is clipped too.
- Laneway core sRGB: opposed 193 197 209; white 208 208 208; from rim 206 207 208; per region +
  from rim 216 206 209; + rim fix + distance dome 226 218 209; gradient domain 255 243 232.
- Fully clipped pixels against the truth (`/tmp/hl-controls/test/`, hue_test.py: median
  chromaticity distance; deficit_test.py: median log2 luminance deficit), opposed / white / from rim
  / per region + from rim + rim fix + distance dome:
  hue g065 0.19 / 0.15 / 0.17 / 0.06, g213 0.14 / 0.09 / 0.12 / 0.10, q08 0.26 / 0.08 / 0.23 /
  0.22, g294 0.41 / 0.32 / 0.37 / 0.23, t60 0034 0.27 / 0.09 / 0.24 / 0.03, g298 0.37 / 0.15 /
  0.36 / 0.16; deficit g065 0.64 / 0.42 / 0.57 / -0.49, g213 0.39 / 0.33 / 0.36 / 0.33, q08 0.11
  / 0.03 / 0.05 / -0.08, g294 0.79 / 0.62 / 0.73 / 0.39, t60 0.44 / -0.26 / 0.38 / -0.21, g298
  0.63 / 0.24 / 0.61 / 0.23.
- Visual (`sheet-color.jpg`, `sky-rim2.jpg`): white turns g065's sunset sky gray; the combination
  gives g065 a salmon-pink sky (truth peach, opposed mauve): Kofa to judge.
- Defaults and all earlier settings (v6 / v7 sidecars) render identically. Not reviewed by Sol
  (Codex budget); not run on X-Trans or a GPU.

## Kofa's GUI test on Laneway, 2026-10-26 (exports `_04`-`_12`)

Exports in `/tmp/gradient-domain-eval/iain/darktable_exported/`, all at exposure -4 EV, AgX off,
color calibration off; settings decoded from the embedded history (`/tmp/hl-controls/rr/hist*.json`);
crops in `/tmp/hl-controls/rr/out/k*.jpg`. Each step changes one control of the previous one.

- `_04` defaults: lavender-gray core, gray-green band, olive glow. The band is likely issue 30's
  two-clipped ring (red estimated from clipped green), not checked on raw values.
- `_05` reference passes 11 (warm lights): band lifted, now a bright ring around a gray core (the
  passes do not touch fully clipped cells).
- `_06` + cube-root offset: brighter glow, more contrast on the wall, magenta core.
- `_07` + chroma offset per region: warmer glow, no olive. `_08` (= `_07` without cube root): most
  of the gain gone, so the two only help together.
- `_09` + fully clipped color white: neutral but gray core (darker than its ring), green-cyan line
  along the core's bottom edge (selection boundary; cause not checked).
- `_10` from rim instead of white: warm, brighter core (raised by blue's clip floor), faint edge line.
- `_11` + brightness fill flat lift: lamp unchanged (cannot rise above the rim); the round sign's
  dark lettering fades: "merge clipped specks" (64) adds the partly clipped strokes to the selection.
- `_12` specks 0: strokes back; the sign turns near neutral, closer to the truth (white with green
  lettering, unrecoverable), its warm edge in `_10`/`_11` came from the lamp-lit outer rim.

Open: speck merge default (0, or only specks as bright as their surroundings); the edge line of the
color step; distance dome not yet tried in the GUI.
- `_13`-`_15` distance dome (strength up to 4, max rise up to 4): no effect on the lamp. Bug in
  my code: the dome's occluder threshold (0.8 x the region's median) used the luminance after
  "from rim" had raised the core, so the bright ring was freed and the rim slope was 0. Fixed in
  8047d90229 (threshold from the luminance before the color step); settings without a color step
  render identically. Laneway with the fix: strength 1 a mild lift, strength 4 / max rise 4 a bright
  warm-yellow core (`/tmp/hl-controls/rr/dome/fix-variants.jpg`). The edge line remains.

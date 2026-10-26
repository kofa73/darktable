# Ideas from RapidRAW's opposed-style reconstruction

Status: pending (2026-10-26; ideas collected, none tried). Kind: experiment candidates.

## Goal

Find out whether parts of RapidRAW's highlight reconstruction improve inpaint opposed with the
controls of issue 31. Kofa tested RapidRAW: overall worse than opposed with issue 31's controls.

## Context

Source: `/workspace/RapidRAW/src-tauri/src/highlight_reconstruction.rs` (commits `28698797`,
`07f1b60d`, 2026-10-20), called from `raw_processing.rs:385-402`. It works on demosaiced,
white-balanced camera RGB (not on the CFA as darktable does), in three steps:

1. Per-channel clip levels from a histogram plateau (`detect_clip_levels`), not from the white
   level in the metadata.
2. Opposed estimate as in darktable (mean of the other two channels in cube-root space, linear
   offset), with differences listed below.
3. A color fill of fully clipped pixels (`fill_blown`).

## Differences from darktable's opposed, and whether they are new to us

| # | RapidRAW | darktable / our tests | new? |
|---|---|---|---|
| a | offset samples weighted by `(value / clip)^2` (brighter samples count more) | plain mean (`opposed.c:552-556`) | yes |
| b | sample band up to 24 px from the clipped area, the nearest 2 px excluded | dilated mask, about 3 mask cells, nearest included (`opposed.c:82`) | partly: issue 11 excluded a fringe ring for gradient domain (negative) |
| c | offset as a spatial field: push-pull pyramid, local mean where samples are dense, coarser mean elsewhere | image-wide or per region (issue 9); Laplace field OFS-LIN offline 50 / 33 / 20 against opposed (issue 27) | partly: the push-pull fallback to coarser levels was not tried |
| d | estimate faded in over 0.90-0.985 of the clip level (smoothstep), also on unclipped pixels | hard switch at the clip level, `max(value, estimate)` | yes; likely aimed at demosaic contamination near clipped pixels, which darktable avoids by working on the CFA |
| e | 3 reference iterations on every partly clipped pixel | issue 30: iterating everywhere is worse than warm lights only | no |
| f | chroma of the estimate pulled toward the pixel mean when the unclipped channels are near their clip too: kept share 0.35 at zero headroom, 1 at headroom >= 0.3 | nothing for partly clipped pixels; "fully clipped color: white" acts on fully clipped cells only (issue 31) | yes |
| g | fully clipped color from pixels with exactly one clipped channel (after step 2), weighted toward neutral samples, spread with a large push-pull support (image size / 28) | "from rim": bright cells around the region, Laplace inward | partly: different sources and weights |
| h | that color applied at half strength (`log` chroma * 0.5), dropped entirely for saturated sources (log spread 0.25-0.6) | white or full rim color, nothing between | yes |
| i | clip level detected per channel from the data | white level from metadata times the clip slider (0.987 for opposed, `highlights.c:56`) | yes, but not about hue: a diagnostic for wrong white levels |

Both programs keep each channel at or above its measured value (`max(observed, ...)`), so f and h
have the floor problem found in issue 31: a channel clipped high after white balance (blue on the
Laneway Nikon) cannot drop to neutral.

## Candidates, by expected value and cost

1. **h: fully clipped color strength** (a slider from white to "from rim"). In issue 31 white
   gave the best hue on 5 of 6 pairs but turned g065's sunset gray; a middle value addresses that
   trade-off directly. Cheap: one blend in `opposed_fill.c`. The saturation cutoff would push the
   sunset back to white, so try it separately, if at all.
2. **f: headroom chroma fade on partly clipped pixels**. Aimed at the colored ring and core of
   lamps. Risk: desaturates partly clipped colored highlights (sunsets, colored lights).
3. **a: brightness-weighted offset samples**, image-wide and per region. Cheap; effect unknown.
4. ~~g: one-clipped-channel pixels as the source~~ dropped (Kofa): after the opposed step their
   clipped channel is opposed's estimate, so their color is opposed's guess, not data. Our "from
   rim" has the same circularity: it reads the rim from the opposed output and excludes only dark
   cells (`opposed_fill.c:1018-1046`), which fits its hue error staying close to opposed's (issue
   31). Variant worth trying instead: rim color from cells with no clipped channel only.
5. **c: push-pull offset field.** Lower: per region (issue 9) and the Laplace field (issue 27)
   cover most of it.
6. **i: plateau detection as a check** on the 103 pairs: report pairs whose detected clip level
   differs from the white level. A finding would be an unrelated darktable bug
   (`/tmp/dt-issues/`), not a change here.

Not worth trying: d (demosaic-order artifact) and e (done in issue 30).

## Visual comparison with RapidRAW's exports (2026-10-26)

Kofa exported 5 images from RapidRAW (exposure -2 EV, "basic" tone mapper, 16-bit TIFF; the
`rr/` directories next to the raws). `/tmp/hl-controls/rr/sheet.py` (copy in `tools/rapidraw-compare/`) maps the darktable renders
(issue 31's `c-def` opposed, `c-white`, `c-lrr` = per region + from rim + rim fix + distance dome;
the truth frame) into the TIFF's linear sRGB scale with per-channel gains fitted on unclipped
mid-tones (gains equal within 5%), aligns by phase correlation and shows all through RapidRAW's
display curve. Laneway rendered fresh (`render_laneway.py`, ref 1 as truth). Output
`/tmp/hl-controls/rr/out/`: `full-*.jpg`, `crop-*.jpg`.

- t60 lamp: RapidRAW neutral white with a soft edge; opposed blue, white gray with a hard blue rim,
  ours pale cyan with a blue spot. Truth core clipped itself, its halo neutral.
- g294 wall: truth warm beige; RapidRAW cream (closest), ours pink, white gray-lavender, opposed
  lavender.
- Laneway lamp: ours warm white (closest); RapidRAW light blue like opposed, wall slightly paler.
- g213 sky: RapidRAW neutral light gray, slightly green; white gray; ours pink (rejected by the
  magenta rule); truth blue sky (its pink patch is the truth frame's own clipping).
- g065 sunset: RapidRAW gray like white (the saturation cutoff drops the rim color); ours
  salmon-pink; truth peach.
- No RapidRAW crop is display-clipped; ours is on t60 (10% of the crop) and g065 (36%).

Reading: RapidRAW wins where the partly clipped ring meets the core (t60, g294). That is f (the
headroom fade pulls nearly clipped pixels toward neutral, so no colored rim) plus the soft `full`
weight of the fill, and h's half strength (g294 cream rather than our pink). It loses saturated
skies (g065) by the cutoff. Supports trying f together with white or a half-strength rim color.

## Done means

Kofa has chosen which candidates to try; each tried one is scored at -2 / -3 EV (issue 28) with
sky crops checked for magenta or cyan, and its result recorded here.

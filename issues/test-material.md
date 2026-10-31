# Missing test material

Status: pending (2026-10-31: X-Trans supplied and scored, see below; no linear raw yet). Kind: data.

- No linear raw (sraw, linear DNG) exists on this machine. Needs Kofa.
- No X-Trans photo with real clipping existed until 2026-10-31 (`/tmp/hl-article/HANDOFF.md`,
  "Current state"). The gradient domain X-Trans plane fit stays untested on real highlights: that
  work stopped on 2026-10-24.
- Possible external data, availability unchecked (research HANDOFF 2.1-2.2): Fu et al. 2023 raw
  over-exposure sets RPO (650 scenes, Canon 5D Mark IV) and SOF (synthetic, from FiveK, about 94 GB on
  Baidu Pan); Laval photometric indoor dataset (11,060 light sources with CCT) as a prior on lamp
  colors.

Done when Kofa has supplied or declined the raws, and the datasets' availability is known.

## Dataset availability (checked 2026-10-23, web search only, no download attempted)

- Fu et al., "Raw Image Based Over-Exposure Correction Using Channel-Guidance Strategy" (CGNet,
  IEEE TCSVT, DOI 10.1109/tcsvt.2023.3311766): paired raw (Canon CR2) / sRGB benchmarks, synthetic
  and real. No public download or repository found; the article is not open access. The CVPR 2024
  PBDL challenge report (arXiv 2406.10744) used CGNet's training data (CR2 converted to RGGB PNG),
  so it was distributed to challenge participants. Next step: ask the authors (Beijing Institute of
  Technology). Unverified whether the real set has a less exposed frame per scene as a truth.
- Laval Photometric Indoor HDR Dataset (Bolduc et al., ICCV 2023): https://lvsn.github.io/beyondthepixel/
  and http://www.hdrdb.com/indoor_hdr_photometric/. Merged HDR panoramas in EXR (not raws with
  clipping), calibrated with a chroma meter; full set after signing a license (non-profit or
  educational), 100 samples free. Useful only as a prior on lamp colors, not as test raws.

## Raws supplied by Kofa (2026-10-31)

- `/tmp/gradient-domain-eval/raw/x-trans-pack/`: 199 RAF, 197 X-T3 and 2 X-Pro2, April 2022, mostly
  in-camera brackets of 5 frames in 2.67 EV steps (ISO 200 to 12800). Daylight scenes: flowers,
  foliage, skin, skies, a playground, a mural, and a rainbow gradient (x37, DSCF4281), which suits
  false color checks. No night or lamp scenes.
- `/tmp/gradient-domain-eval/bracketed/x-trans/`: 3 RAF, X-T20, 1 EV steps, clouds over hills. The
  frames have different as-shot white balance (blue up to 0.33 EV apart), so their color cannot be
  compared with the truth frame.
- `/tmp/gradient-domain-eval/raw/4Bayer/`: Sony DSC-F828 (RGBE) and Canon PowerShot G1 (CYGM). Not
  usable for this work: highlight reconstruction falls back to clipping on 4-color sensors
  (`src/iop/highlights.c:1240`, `:1405`, `:1547`). Rendered with inpaint opposed and with the
  candidate settings, both are bit-identical to clipping. No crash.

Results in `opposed-controls-build.md`, section on X-Trans.

# Missing test material

Status: pending. Kind: data.

- No X-Trans photo with real clipping and no linear raw (sraw, linear DNG) exist on this machine
  (`/tmp/hl-article/HANDOFF.md`, "Current state"). The X-Trans plane fit is untested on real
  highlights. Needs Kofa.
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

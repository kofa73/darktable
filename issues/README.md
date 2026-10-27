# Issues

Bugs to fix and experiments to try, sorted by priority (highest first). Status: wip, pending or
done; at most one item is wip at a time. Background: `/tmp/hl-article-research-1/HANDOFF.md`
(research and scoring, "HANDOFF 3.6" etc.) and `/tmp/hl-article/HANDOFF.md` (design notebook).
Gradient domain stopped on 2026-10-24 (Kofa): what stays valid is in [learnings.md](learnings.md);
reusable code is in [salvage/](salvage/README.md), the evaluation scripts in `tools/`, agents' notes in
`agent-notes/`; the deleted branch is archived locally only (`/tmp/hl-archive/gradient-hl.bundle`).
State and next steps of the latest session: [HANDOFF.md](HANDOFF.md) (updated 2026-10-27).

| # | file | description | status |
|---|---|---|---|
| 1 | [tooling-fixes.md](tooling-fixes.md) | evaluation scripts: hidden render failures, pair name collision, noisy motion figure | done |
| 2 | [score-remaining-brackets.md](score-remaining-brackets.md) | all brackets as real 1-2 EV pairs; crop sheet of light cores without a truth | done (closed: gradient domain stopped) |
| 3 | [per-region-method-choice.md](per-region-method-choice.md) | inpaint opposed for skies, gradient domain for lights, chosen per region | done |
| 4 | [border-classification.md](border-classification.md) | sky versus occluder border segments; fill foliage gaps from open sky | done (negative) |
| 5 | [partly-clipped-sky-tint.md](partly-clipped-sky-tint.md) | magenta/cyan tint on partly clipped near-neutral clouds and skies | done (closed: gradient domain stopped) |
| 6 | [rescore-fills-and-selectors.md](rescore-fills-and-selectors.md) | edge fill (13, 14) and selectors A-D with the new score; redone at reduced resolution | done (closed: gradient domain stopped) |
| 7 | [holding-and-writeback.md](holding-and-writeback.md) | holding everywhere or reduced grid only; write-back parameter now a no-op; redone at reduced resolution | done (closed: gradient domain stopped) |
| 8 | [hue-estimate-channel-bound.md](hue-estimate-channel-bound.md) | clipped channel's lower bound in the hue estimate for all flagged pixels | done (negative) |
| 9 | [opposed-local-chroma.md](opposed-local-chroma.md) | inpaint opposed with per-region chroma offsets | done (modest gain) |
| 10 | [dome-for-clipped-cores.md](dome-for-clipped-cores.md) | let fully clipped lights rise above their rim | done (closed: gradient domain stopped; the dome continues on inpaint opposed, issue 29) |
| 11 | [fringe-color-sources.md](fringe-color-sources.md) | keep fringe pixels out of the color sources | done (negative) |
| 12 | [segmentation-auto-settings.md](segmentation-auto-settings.md) | per-image combine and candidating for segmentation based | done (small gain) |
| 13 | [directional-declipping.md](directional-declipping.md) | four-direction 1D declipping; first as an error predictor | done (negative) |
| 14 | [hue-from-selected-location.md](hue-from-selected-location.md) | region hue from one smooth, bright location instead of a blend | done (negative) |
| 15 | [solver-accuracy-and-speed.md](solver-accuracy-and-speed.md) | CG tolerance dependence, early stop on large regions, OpenMP spin wait | done (multigrid solver in the branch, `6ea0e47cd6`) |
| 16 | [small-code-fixes.md](small-code-fixes.md) | stale write-back comment, no-border early return, smooth fill cap | done |
| 17 | [merge-readiness.md](merge-readiness.md) | Dolby patent, which values stay, rawprepare patch, release notes | done (closed: gradient domain stopped, no merge request) |
| 18 | [rebase-onto-master.md](rebase-onto-master.md) | branch lacks the #22436 `-t` fix; workaround OMP_NUM_THREADS | done (closed: gradient domain stopped) |
| 19 | [restore-g289-raw.md](restore-g289-raw.md) | truncated raw P1080098.RW2, needs Kofa | done |
| 20 | [test-material.md](test-material.md) | no X-Trans with real clipping, no linear raw; external datasets unchecked | pending |
| 21 | [metric-replacement.md](metric-replacement.md) | SSIMULACRA 2 on aligned real pairs replaces mean dE | done |
| 22 | [hanno-reports.md](hanno-reports.md) | segmentation based / inpaint opposed bugs and question, tested | done |
| 23 | [rules-study.md](rules-study.md) | fill selectors A-G for gradient domain (mean dE, superseded) | done |
| 24 | [full-resolution-speed.md](full-resolution-speed.md) | full resolution under 1 minute of CPU time with the multigrid solver | done (closed: gradient domain stopped; round-6 candidate not ported) |
| 25 | [local-regression-predictor.md](local-regression-predictor.md) | clipped channels from a local regression on the measured channels (research of 2026-10-24) | done (negative offline) |
| 26 | [selftest-method-choice.md](selftest-method-choice.md) | choose inpaint opposed or gradient domain per image (self-test, clipped-share rule) | done (closed: gradient domain stopped) |
| 27 | [opposed-hue-blend.md](opposed-hue-blend.md) | geometric blend of inpaint opposed and gradient domain; opposed offset as a smooth field (scores favored it; Kofa: magenta in skies, opposed looks better) | done (negative) |
| 28 | [score-display-clipping.md](score-display-clipping.md) | score and crop sheets clip each sRGB channel at 0 / -1 EV: a third to half of the truth in clipped areas is white or hue-shifted | pending (rescored at -2 / -3 EV: gradient domain 41 / 39 against opposed, skies 5 / 4) |
| 29 | [opposed-brightness.md](opposed-brightness.md) | inpaint opposed: raise the brightness of fully clipped areas (flat lift from the rim, dome); offline: distance dome 22 / 7, gains on clouds, not lamps; brighter wrong-hued skies; Kofa chose (c): issue 30 first | pending |
| 30 | [opposed-two-clipped-ring.md](opposed-two-clipped-ring.md) | inpaint opposed: partly clipped pixels too dark where red and green clip (g294 0.5 EV); fix: iterate the references for warm lights only, 4 / 1 alone, with issue 29's dome 27 / 7; waits for Kofa | done |
| 31 | [opposed-controls-build.md](opposed-controls-build.md) | experiment build with GUI controls for inpaint opposed (branch `opposed-controls`, params v8): rim fix, cube-root and per-region chroma, fully clipped color, brightness fill with dome fixes (8047d90229, 28d108271e); glow test reverted; Laneway settings on 103 pairs: 62 / 7 without the dome, but the cube-root offset makes skies pink; dome at strength 4 a net loss; candidate: passes 11, per region, from rim, flat lift (no cube root), 56 / 4 | wip |
| 32 | [rapidraw-ideas.md](rapidraw-ideas.md) | ideas from RapidRAW's opposed variant; its exports beat ours on t60 and g294 (neutral, no colored rim), lose on g065; candidates: headroom chroma fade, fully clipped color strength, brightness-weighted offsets; none tried | pending |
| 33 | [early-literature-ideas.md](early-literature-ideas.md) | unpatented ideas from the early literature: transition band between 1/2/3-channel clipped pixels (edge line), free-center 2D Gaussian fit for fully clipped brightness (Xu et al. 2011); none tried | pending |

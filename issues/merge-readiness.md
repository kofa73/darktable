# Decisions and cleanup before a merge request

Status: done (2026-10-24, closed: Kofa stopped the gradient domain work; see learnings.md). Before: pending. Kind: decision (Kofa).

1. **Dolby patent** US8861851B2 (Heidrich, Rouf), listed active to 2032-11-30 by Google Patents (not
   a legal check): gradient domain follows its method (research HANDOFF 2). Decide before proposing
   a merge.
2. **Which experiment values stay.** Best fixed fill so far: "continuous border, hue estimate in
   fully clipped". Kofa's verdicts on values 10, 11, 14 and 15 are open; see also
   `rescore-fills-and-selectors.md`.
3. **The write-back parameter**: see `holding-and-writeback.md`.
4. Label "gradient domain" and file name; fallback to inpaint opposed in fast pipes; CPU only, no
   OpenCL and no tiling (`dev-doc/iop/highlight/gradient_domain.md:21`).
5. **Working tree:** revert the synthetic clipping in `src/iop/rawprepare.c`
   (`git apply -R /tmp/claude-gd-artefacts/synth.patch`) once no synthetic runs are planned; the
   untracked `fix-plan*.md` review notes stay out of any commit.
6. `RELEASE_NOTES.md` entry, integration test for the new method, rebase (`rebase-onto-master.md`).

Source: `/tmp/hl-article/HANDOFF.md`, "Open questions"; `/tmp/hl-article-research-1/HANDOFF.md` 4.7.

## Inputs from 2026-10-23 (issues 2-16)

- Item 2: scored on 103 real pairs, "continuous border, hue estimate in fully clipped" (value 10)
  remains the best fill: "white in fully clipped" (11) ties it (8 wins, 5 losses, 90 ties), losing on
  colored lights; "edge hue to white" (14) and the measured-channels edge fill (13) are worse and make
  lamps purple-pink; "opposed's estimate as hue" (9) mostly reproduces inpaint opposed and loses the
  lamp wins. Crop sheets await Kofa (`rescore-fills-and-selectors.md`).
- Item 3: measured (`holding-and-writeback.md`); recommendation keep holding everywhere and drop the
  write-back parameter; Kofa's decision.
- Item 4 (run time): at full resolution the module takes up to 154 s wall / 1066 s CPU on a
  20 MP sky; at reduced resolution up to 32 s / 152 s (`solver-accuracy-and-speed.md`). Kofa's
  limit is 1 minute; the solver needs work before a merge (issue 15, in progress).
- Patent-free alternative found: inpaint opposed with one chroma offset per clipped region
  (`opposed-local-chroma.md`): 16 wins, 6 losses against plain opposed, no slider, fast. It does not
  reconstruct fully clipped lamp cores, where gradient domain wins.

## Inputs from 2026-10-24

- Item 4 (run time): the multigrid solver is in the branch (`6ea0e47cd6`); reduced resolution now
  takes at most 7.6 s wall / 39 s CPU on the 10 timing pairs, full resolution still up to 198 s CPU
  (issue 24).
- Item 1 (patent), from Sol's research (`/tmp/hl-rules-study/tasks/2026-10-24-research/answer-sol.md`),
  checked against Google Patents' claim list of US8861851B2 (summary of the page, not the verbatim
  claims): besides claim 11 (gradients from the measured channels, weighted combination), the
  independent claims 20 and 42 cover reconstructing partly saturated pixels by multiplying the
  unsaturated channels' values by ratios implied by the estimated hues; claim 1 covers hue in-painting
  from the boundary with a proximity- and brightness-weighted 1-D filter. So replacing the Poisson
  solve alone does not leave the patent: a hue-ratio fill of partly clipped pixels is claimed by
  itself. Not a legal assessment. Sol also lists a later Dolby family, US11074677B2 / EP3547251B1
  (McElvain, Atkins, "Dynamic range extension of partially clipped pixels"), active, which groups
  unclipped colors and estimates missing channels from cluster information; and US9912839B2 /
  EP3100449B1 (Abebe et al., reference-channel variation added above a regional minimum), active.
  Gemini's answer says the ratio-based alternatives are "free of US8861851B2 claims"; that
  contradicts the claim list and should not be relied on.
- Item 4 (OpenCL), HANDOFF step 4: Gemini's feasibility note,
  `/tmp/hl-rules-study/tasks/2026-10-24-opencl-note/answer.md` (design only, nothing built). Its
  citations I sampled are right (`highlights.c:1205`, `opencl.c:708-716`, `opencl.c:316-322`; no
  kernel in `data/kernels/` computes in double, `spektrafilm.cl` mentions it only in comments).
  Main points: level 0 as a dense grid with implicit neighbors, V-cycle in float and CG vectors,
  dot products and the gauge restore in double (`cl_khr_fp64` is an extension in OpenCL 1.2, so a
  CPU fallback is needed); device memory for 24 MP about 2.3 GB at full resolution with 30%
  clipped, up to 5.6 GB fully clipped, under 1.5 GB at reduced resolution; about 37,000 kernel
  launches per full-resolution image; estimated 7-10x faster than the CPU on a mid-range GPU and
  35-48 developer days (its estimates, inferred, not measured). Its recommendation: do not port
  while the method is experimental and reduced resolution runs in 3-8 s on the CPU.

## Patent claims, verbatim reading (2026-10-24)

Read from the granted patent PDF (`/tmp/patent-dl/US8861851.pdf`, text in `claims.txt` there;
priority date 2011-05-13). Independent claims: 1, 11, 20 (methods), 22, 41, 42 (apparatus), 45
(medium). Each independent claim is narrower than "fill the hue from the border":

- Claim 1: in-painting hues from the boundary *and* "applying a 1-dimensional spatial filter along
  the boundary prior to in-painting the hues", weighted by proximity, with less proximity effect for
  a current pixel with small values. Our `_gd_smooth_border()` (`gradient.c:1015`) says it smooths
  "like the 1D filter of the paper"; that is the part of the branch closest to claim 1.
- Claim 11: gradients of saturated values estimated from non-saturated gradients, "based in part on
  the estimated hues", and "a weighted combination of gradients estimated from the known gradients
  for the plurality of color channels": the guided Poisson solve.
- Claim 20: for partly saturated pixels, "multiplying values based on the pixel values for one or
  more non-saturated color channels by values based on the estimated hues", the latter being "ratios
  of saturated channels to non-saturated channels". The broadest claim.

Kofa's question: do guided laplacians (darktable) or color propagation (RawTherapee) do the same?
Guided laplacians (`laplacian.c`, 2022) diffuses RGB ratios and a norm over wavelet scales and
rebuilds RGB as ratios times norm; no boundary 1-D filter, no hue-guided gradient combination per
channel. Whether ratio-times-norm reads on claim 20 is a legal question. RawTherapee's highlight
reconstruction source header dates the code to 2011-06-16, a month after the priority date, so by
that date alone it is not prior art (earlier publications not checked). Not checked: other prior
art for claim 20 (cross-channel methods such as Zhang and Brainard 2004).

Patent family (Google Patents, 2026-10-24): US only. US 13/463,775 (granted as US8861851B2, published
as US20120288192A1), provisional US 61/486,228; no EP, WO, CN or JP member. Later EP / CN documents
only cite it. Expiry listed as 2032-11-30 (adjusted), Google's estimate.

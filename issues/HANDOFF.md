# Handoff (2026-10-24, updated 14:50 UTC)

State of the highlight reconstruction work after the sessions of 2026-10-23/24, for the next
session. The issue files in this directory hold the details and results; this file says where
things are, how the tooling works, and what to do next. Read `README.md` (the index) and this file
first, then the issue file you work on.

## 0. Decisions of 2026-10-24, afternoon (Kofa)

- **Gradient domain work stops**: Dolby's patent (issue 17, claims read verbatim) and its magenta
  skies (issues 5, 27). Its issues are closed; what stays valid is in [learnings.md](learnings.md).
  The branch `gradient-hl` and its commits stay as they are; nothing is reverted.
- **Scoring at -2 / -3 EV** (issue 28): at 0 / -1 EV the per-channel display clipping hides hue
  errors in the clipped areas. A score win on skies needs a visual check for magenta or cyan.
- **Next direction: inpaint opposed's brightness** in fully clipped areas (issue 29, wip): a flat
  lift from the rim and a dome, opposed's hue kept; offline prototype on the existing renders first.

- **Update 2026-10-24 evening (Kofa away; issue 29):** the salvage domes tried on inpaint opposed,
  offline on its renders. Best: a flat lift plus a distance-based dome (occluder rim pixels free,
  2 EV soft limit, clipped specks merged): 22 / 7 against opposed at -2 / -3 EV, median deficit of
  fully clipped pixels 0.20 -> 0.08 EV; the gains are on clouds and glare, not on lamps, whose
  partly clipped ring is already too dark. It brightens opposed's wrong sky hues too (beige, lilac).
  On top of local chroma offsets it scores 35 / 9 but the skies turn pink-magenta. Decision for
  Kofa at the end of `opposed-brightness.md`.

- **Update 2026-10-25 (Kofa chose option (c), issue 30):** opposed's dark ring on warm lights comes
  from two clipped channels clamping each other's reference; iterating the references where red
  and green clip and blue is measured fixes it (4 / 1 against opposed, no tint) and lets issue 29's
  dome reach the lamps: together 27 / 7. Experiment copy and build `/tmp/hl-rules-study/dt-src`,
  `dt-build` (switches `DT_OPP_ITER`, `DT_OPP_ITER_RG`, `DT_OPP_CBRT`). Decision for Kofa at the end
  of `opposed-two-clipped-ring.md`.

Sections 1-5 below describe the gradient domain work as it was; they are kept for reference.

## 1. State in one paragraph

Issues 1-16 were worked through. Done or negative: 1, 3, 4, 8, 9 (modest gain), 11, 12, 13, 14, 16.
Waiting for Kofa: 2, 6, 10 (crop sheets to judge), 7 (holding / write-back decision), 15 (port of
the fast solver), 17-18, 20. Parked by Kofa's choice: 5 (sky tint). Main findings: gradient domain
("continuous border, hue estimate in fully clipped", script name `gd-wb4x10`) wins on lamps and night
scenes and loses on daylight skies, which is the paper's own stated limit (Rouf 2012 section 3,
Figure 6); reduced resolution ("full resolution" off) scores better than full resolution and is
4-10x faster; a multigrid-preconditioned CG by GPT-6.1 Sol keeps the current quality at reduced
resolution within 1 minute of CPU time on all 103 pairs (issue 15, items 6-10).

Update 2026-10-24 (next steps 1 and 2 done): the comparisons for Kofa were redone at reduced
resolution (issues 2, 6, 7, 10: tables and crop sheets; at reduced resolution holding beats no
holding, 19 against 6 pairs, and the fills rank as at full resolution), and Sol's round-4 solver is
in the experiment copy behind `DT_GD_MG=1` (issue 15, item 11): byte-identical when off, the same
scores as Sol's renders on all 103 pairs when on, reduced resolution median 2.7 s wall / 11 s CPU,
maximum 7.6 s / 39 s on the 10 timed pairs. All of 2, 6, 7, 10, 15 now wait for Kofa.

Update 2026-10-24, later (Kofa: "port sol's solver. for now, keep the full-res mode"; then away,
work autonomously): the multigrid solver is the branch's solver (commit `6ea0e47cd6`, issue 15 item
12), slimmed to about 175-190 bytes per unknown with byte-identical output (checked single-threaded:
at more threads, OpenMP reduction order makes repeated runs differ in the last bits). Full
resolution with it scores better than with the old CG (15 / 3 / 85) but still takes up to 198 s CPU
on skies: issue 24 (wip, Sol round 5). Research on new ideas by Sol and Gemini:
`/tmp/hl-rules-study/tasks/2026-10-24-research/`.

Update 2026-10-24, 08:00-11:00 UTC (Kofa away): issue 24, Sol round 5: the solver is memory-bound;
a cap of 1-2 solver threads meets the CPU limit (Kofa's decision); round 6 (memory traffic) found
a candidate with 9-14% less wall time at 2 solver threads, 13-22% at 12, 16% less memory, same scores
(103 ties): port it? (Kofa's decision). Issue 25 (local regression):
negative offline. Issue 26: a per-image switch on the clipped share beats both methods (Kofa's
decision). Phone pairs: the DNG GainMap causes false clip flags (duplicate of darktable #12128,
note in `/tmp/dt-issues/duplicates/`). New issue 27: **the geometric blend
`opposed^0.75 * gradient domain^0.25` of the two methods' outputs beats inpaint opposed 30 / 12 on
the 103 pairs** (darktable prototype `DT_HL_BLEND=0.25`, raw path); the weight works like a strength
slider (0.15: 23 / 5, 0.35: 35 / 16); with full-resolution gradient domain in the blend, 34 / 12
(the first gain from the full-resolution mode); without the 9 phone pairs (GainMap tint) 21 / 12 and
25 / 12. Crop sheets for Kofa: `/tmp/hl-rules-study/runs/blend25-*` and `fblend25-*`.

## 2. Next steps (agreed with Kofa 2026-10-24: go ahead with 1 and 2; 3 only if Kofa says so)

Steps 1 and 2 are done (2026-10-24, see the update in section 1); their text is kept for reference.

1. Redo, at reduced resolution, the comparisons Kofa still has to judge, so that his decisions rest
   on the likely default configuration:
   - issue 7: holding against no holding with write-back "joint, any channel clipped, capped" and
     "basic" (methods `gd-r-hold`, `gd-r-nohold`; add a basic-write-back reduced variant);
   - issue 6: fills at reduced resolution (hue fill = current `gd-r-hold`; add white fill and edge
     fill reduced variants: `['6', '--wb', '4', '--exp', '11']` and `'14'`);
   - crop sheets of light cores (issues 2, 6, 10) from reduced-resolution renders
     (`scripts/crop_sheet.py`, `scripts/dome.py` with `DOME_SRC`).
2. Port Sol's round-4 solver (`/tmp/hl-rules-study/tasks/2026-10-23-solver-speed/mg_solver_round4.inc`,
   built by `build_round4.py` there) into the experiment copy `/tmp/hl-rules-study/dt-src` behind an
   environment switch (e.g. `DT_GD_MG=1`), so that later experiments run fast. Luna's earlier port
   of the round-2 solver (`/tmp/hl-rules-study/tasks/2026-10-23-mg-port/`, switch `DT_GD_MG`,
   `DT_GD_MG_SMOOTH=rbgs`, build script `build_mg.py`) shows how; it must get round 3's gauge (floating
   components keep their degree-weighted start mean) and round 4's changes. Verify: switch off =
   byte-identical to current renders; switch on = within run-to-run noise of `gd-r-mg3.pfm`
   (round 3 differs from itself by ~1e-3 mean on t25/t27), and identical scores. Do not touch the
   branch `/workspace/darktable` for this.
3. Only if Kofa agrees: issue 5 candidates (Masood's edge-aware hue estimate; color trend against
   log brightness), as experiments.
4. Optional: an OpenCL feasibility note for the solver (design and cost estimate only). Done
   2026-10-24 by Gemini, see `merge-readiness.md`, inputs of 2026-10-24.

## 3. Where things are

- Branch `gradient-hl` in `/workspace/darktable`. Last commit `6ea0e47cd6` (multigrid solver, issue 15).
  Uncommitted: `CLAUDE.md` (sections "Commits and branches", "Temporary files" added this session;
  ask Kofa before committing it), `src/iop/rawprepare.c` (synthetic clipping hook, TEMPDEBUG; revert
  before a merge, issue 17), untracked `issues/`, `fix-plan*.md`. CLAUDE.md allows commits on this
  branch and on sub-branches; pushing still needs Kofa.
- Experiment copy: source `/tmp/hl-rules-study/dt-src` (gradient.c before the multigrid port:
  `tasks/2026-10-24-mg-port4/gradient-before-port.c`), build `/tmp/hl-rules-study/dt-build`
  (`cd /tmp/hl-rules-study/dt-build && nice -n 19 cmake --build . -j 12`). All experiment switches
  are environment variables read in `src/iop/hlreconstruct/gradient.c` and `opposed.c`, off by
  default (TEMPDEBUG blocks): DT_GD_MG (multigrid solver, issue 15 item 11), DT_GD_OCCLUDER(_RAD), DT_GD_OPP_TWO, DT_GD_OPP_LOCAL, DT_GD_NO_HOLD,
  DT_GD_HUE_BOUND, DT_GD_FRINGE(_RAD), DT_GD_CG_TOL, DT_GD_CG_MAXITER, DT_GD_HUE_PYR, DT_GD_HUE_PP,
  DT_OPP_LOCAL, DT_NO_GAINMAP (rawprepare: DNG GainMap off), DT_GD_HUE_BLEND (gradient.c), DT_HL_BLEND
  (highlights.c, issue 27), plus the older DT_OPP_*/DT_SEG_* of the segmentation study. A rebuild replaces
  `libhighlights.so` that every render of that build uses: never rebuild while a render run uses it.
- Real pairs: 141 pairs from Kofa's brackets (`/tmp/gradient-domain-eval/bracketed/`), plan
  `/tmp/hl-rules-study/plans/allpairs.json`, workdirs `/tmp/hl-rules-study/runs/allpairs/<pair>/`.
  The 103 scored pairs (motion <= 15%, truth for >= 50% of clipped pixels) are listed in
  `/tmp/hl-rules-study/scratch/included.txt` (one `allpairs/<pair>` per line, relative to `runs/`).
  Five light-core scenes without a 1-2 EV pair: `runs/cores5/`. Night/indoor list for sheets:
  `scratch/sheetB.txt` (filter to pairs that have the method's PFM).
- Per workdir: `scores.json` (ref, clipped, k), `masks.npz` (F per-channel clip fractions, `all`),
  linear Rec2020 PFMs `truth.pfm`, `none.pfm`, `opposed.pfm`, `gd-wb4x10.pfm` (all 141 pairs) and, for
  the 103 scored pairs, `gd-r-hold` (current code, reduced resolution), `gd-nohold` (no holding, joint capped write-back, full), `gd-wb4x9` ("opposed's
  estimate as hue"), `gd-wb4x11` ("white in fully clipped"), `opp-local` (inpaint opposed with local
  offsets), and since 2026-10-24 `gd-r-nohold` (no holding, reduced), `gd-r-wb11` (white fill,
  reduced), `gd-r-mg4` (ported multigrid solver, reduced; replaces Sol's `gd-r-mg3`, removed); everything else was removed (see the issue files' "Removed" lines). Also
  `align/al_0001.tif` + `align/mk_*.tif` (aligned truth and its warped clipping, regenerated
  2026-10-24) and `pair_align*.json` (scores per run).
- Papers (Kofa's, with `.txt` extracts next to them): `/tmp/hl-article/` (Rouf2012GDC, Multigrid and
  Multilevel Preconditioners, Locally Adapted Hierarchical Basis Preconditioning, Streaming Multigrid,
  Convolution Pyramids, Masood 2009 cross-channel correlation, Multigrid tutorial). Notes on each are
  in `solver-accuracy-and-speed.md` and `partly-clipped-sky-tint.md`.
- Agent task directories: `/tmp/hl-rules-study/tasks/2026-10-23-*` (pink-fill, solver-speed rounds
  1-4, convpyr, gemini-membrane, mg-port). Each has task*.md and answer.md.

## 4. Tooling (all in `/tmp/hl-rules-study/scripts/`; Python `/tmp/claude-gd-artefacts/venv/bin/python`)

- Method definitions: `ev4.py` (on top of `/tmp/claude-gd-artefacts/scripts/ev2.py`): a method =
  make_sidecar2 arguments + environment (`ev2.METHODS[name]`, `ev2.METHOD_ENV[name]`). Mode 6 is
  gradient domain, `--full` = full resolution, `--wb N` write-back, `--exp N` experiment value; mode 5
  inpaint opposed, 4 segmentation based. Add new variants at the end of ev4.py.
- Render extra methods into existing workdirs: `render_extra.py` with `RX_METHODS=a,b`,
  `SYNTH_CFG=<per-worker config copy runs/allpairs/cfg0..3>`, `EV2_THREADS`, `OMP_NUM_THREADS`,
  `OMP_WAIT_POLICY=passive`. Template runner with 4 workers: `runs/run_speed.sh` / `runs/run_iter.sh`
  (edit RX_METHODS, SSIM_METHODS, PA_OUT). Use one config directory per concurrent job (two jobs on
  one config directory fail with "database is locked").
- Score: `pair_align.py <workdir>` with `PA_HALF=1`, `SSIM_METHODS=opposed,...` (all methods to
  compare in ONE run: the changed-pixel mask is their union), `PA_OUT=pair_align-<tag>.json`;
  `PA_ALIGN_ONLY=1` only recreates `align/`. Tables: `pair_table.py <workdirs>` with
  `PT_IN=pair_align-<tag>.json PT_METHODS=A,B` (B against A; tie |d| < 2; motion threshold 15%).
  Method of HANDOFF 3.6 of `/tmp/hl-article-research-1/HANDOFF.md`: SSIMULACRA 2 of each render against
  a reference that is the clipped frame where nothing changed and the aligned darker frame where any
  method changed pixels, linear output, 0 and -1 EV.
- Runners of 2026-10-24: `runs/run_reduced.sh`, `run_reduced2.sh` (step 1), `run_mgport.sh`
  (rebuild with the port, render and score `gd-r-mg4`); `scripts/split_sheet.py` cuts tall crop
  sheets into `*-part<n>.png` between rows.
- Other scripts written this session: `crop_sheet.py` (unrecorded light cores; `SSIM_METHODS`,
  `CS_EVS`, `CS_CROPS`), `region_oracle.py` (per-region choice; `SSIM_METHODS=A,B`, `RO_IN`),
  `border_check.py`, `border_local.py`, `border_select.py`, `dir_declip.py`, `dome.py`, `seg_oracle.py`,
  `hold_singles.py` (single raws, sheets of variants), `gd_timing.py` (idle-machine timing of the
  highlights module via `-d perf`; `GT_ENV`, `GT_TAG`, `GT_MODULEDIR`), `pick_pairs.py`, `pick_cores.py`.
- darktable-cli does not overwrite an existing output; it writes `<name>_01.pfm` etc. ev2.render
  removes the target first; other callers must too.

## 5. Rules learned this session (also in memory and CLAUDE.md)

- Kofa's runtime limit: under 1 minute of CPU time per image for the final method; experiments may
  ignore it. Report run times with results.
- Keep >= 50 GB free on /tmp; no dataset downloads; after each task remove regenerable scratch files
  no later task or decision needs, and note the removal in the issue file. Ask before deleting other
  sessions' files. Free space on 2026-10-24: about 118 GB.
- Name options by their GUI strings ("continuous border, hue estimate in fully clipped"), not
  "experiment 10"; script names like `gd-wb4x10` only alongside.
- Agents: ask-luna (routine work, cheap), ask-codex (GPT-6.1 Sol, hard work), ask-gemini (second
  opinions; `/boost` first line). Verify their claims; tell them where they may write; one agent at a
  time edits a given code base; none commits. Codex scripts: `ask-codex -C <dir> -f task.md`.
- Never use `pgrep -f`/`pkill -f` with a script name; wait on PIDs. PID 1 belongs to this user
  here, so `kill -0 1` is always true.

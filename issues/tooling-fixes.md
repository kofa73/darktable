# Small fixes in the evaluation scripts

Status: done (2026-10-23). Kind: bug (tooling, under /tmp/hl-rules-study/scripts/).

1. `ev4_run.py` / `ev3_run.py` print "done" even when a render failed; check `log.txt` for
   "render failed" before trusting a run. Fix: print the result of ev2 and mark failures.
2. `pick_pairs.py` names a pair by its clipped frame only, so two pairs with the same clipped frame
   share one workdir (t63-stained-glass-0055). Fix: include the truth frame in the name.
3. `pair_align.py` motion figure (share of ring pixels with > 20% luminance difference) is high on
   Half Dome (42%), probably the darker frame's noise in dark areas, not motion. Fix: restrict the
   ring to pixels well above the noise floor, or compare after a small blur.

## Result

1. `ev2_run.py` (in `/tmp/claude-gd-artefacts/scripts/`), `ev3_run.py` and `ev4_run.py` check the
   exit code of ev2 and the presence of `scores.json`. Per entry they print `done` or
   `FAILED <name> (exit n): <last line of log.txt>`, and at the end the list of failed entries,
   with exit status 1. Tested with a plan entry pointing at a missing raw. `synth_run.py`
   (the older synth_eval runner) has the same bug; left as is, it is no longer used.
2. `pick_pairs.py` names pairs `<scene>-<clipped frame>-<truth frame>`, e.g.
   `t63-stained-glass-0055-0050` and `-0055-0051`. Workdirs of earlier runs keep the old names.
3. `pair_align.py` blurs both luminance images with a 2 px Gaussian before comparing. Tested on the
   existing workdirs (old -> new): g319 25.8% -> 2.2%, g213 11.2% -> 0.6%, g437 0453 16.5% -> 3.3%,
   Half Dome 41.9% -> 14.0%, t29 0778 23.5% -> 13.6%. Real motion stays high: g347 dahlia
   56.3% -> 54.7%, t63 97.3% -> 100%. A brightness floor (5% or 10% of the clipping level instead
   of 1%) changed little once blurred. The 14% left on Half Dome lies on the tree and branch
   silhouettes against the clipped sky (`/tmp/hl-rules-study/scratch/halfdome-motion-small.png`):
   branches moving in wind or sub-pixel misalignment at high-contrast edges, not noise. Scores do
   not change (g319 rerun on a copy: identical SSIMULACRA 2 values).

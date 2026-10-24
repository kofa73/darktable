# Replace mean dE with a score that matches the eye

Status: done. Kind: experiment (evaluation method).

- Result: HANDOFF 3.6. SSIMULACRA 2 on linear output, aligned real pairs, changed pixels and seams,
  only where a truth exists. Agrees with all of Kofa's visual verdicts; mean dE and tone-mapped
  output did not.
- Scripts: `/tmp/hl-rules-study/scripts/pair_align.py`, `pick_pairs.py`, `lin_eval.py`,
  `ssim_eval.py`; tool `/tmp/ssimulacra2-src/ssimulacra2/target/release/ssimulacra2_rs`.

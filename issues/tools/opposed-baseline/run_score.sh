#!/bin/bash
# pair_align.py at -2 / -3 EV on the 103 scored pairs (opposed first: pair_align.py aligns on it) for c-def (baseline), c-nodome, c-best;
# then tables of each variant against c-def
cd /tmp/hl-rules-study
T=tasks/2026-10-27-baseline
P=/tmp/claude-gd-artefacts/venv/bin/python
export PA_HALF=1 PA_EVS=-2,-3 PA_OUT=pair_align-base27.json SSIM_METHODS=opposed,c-def,c-nodome,c-best OMP_NUM_THREADS=3
sed 's#^#runs/#' scratch/included.txt | xargs -P 4 -n 1 nice -n 19 $P scripts/pair_align.py > $T/score.log 2>&1
for d in $(sed 's#^#runs/#' scratch/included.txt); do rm -f $d/bl-2_*.png $d/bl-3_*.png; done
for m in c-nodome c-best; do
  PT_IN=pair_align-base27.json PT_EVS=-2,-3 PT_METHODS=c-def,$m $P scripts/pair_table.py $(sed 's#^#runs/#' scratch/included.txt) > $T/table-$m.txt
done
PT_IN=pair_align-base27.json PT_EVS=-2,-3 PT_METHODS=c-nodome,c-best $P scripts/pair_table.py $(sed 's#^#runs/#' scratch/included.txt) > $T/table-dome.txt
echo done

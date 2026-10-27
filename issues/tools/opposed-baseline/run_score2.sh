#!/bin/bash
# pair_align.py at -2 / -3 EV on the 103 pairs (opposed first: it aligns on it) for the "from rim" variants against c-def
# then tables of each variant against c-def
cd /tmp/hl-rules-study
T=tasks/2026-10-27-baseline
P=/tmp/claude-gd-artefacts/venv/bin/python
export PA_HALF=1 PA_EVS=-2,-3 PA_OUT=pair_align-rim27.json SSIM_METHODS=opposed,c-def,c-p11r,c-p11lr,c-p11r-d1,c-p11r-d2,c-nodome OMP_NUM_THREADS=3
sed 's#^#runs/#' scratch/included.txt | xargs -P 4 -n 1 nice -n 19 $P scripts/pair_align.py > $T/score2.log 2>&1
for d in $(sed 's#^#runs/#' scratch/included.txt); do rm -f $d/bl-2_*.png $d/bl-3_*.png; done
D=$(sed 's#^#runs/#' scratch/included.txt)
for m in c-p11r c-p11lr c-p11r-d1 c-p11r-d2 c-nodome; do
  PT_IN=pair_align-rim27.json PT_EVS=-2,-3 PT_METHODS=c-def,$m $P scripts/pair_table.py $D > $T/table2-$m.txt
done
# per region on top of from rim, and the domes on top of from rim
for m in c-p11lr c-p11r-d1 c-p11r-d2; do
  PT_IN=pair_align-rim27.json PT_EVS=-2,-3 PT_METHODS=c-p11r,$m $P scripts/pair_table.py $D > $T/table2-rim-vs-$m.txt
done
echo done

#!/bin/bash
# pair_align.py at -2 / -3 EV on the 103 pairs (opposed first) for Kofa's g065 settings
# then tables of each variant against c-def
cd /tmp/hl-rules-study
T=tasks/2026-10-27-baseline
P=/tmp/claude-gd-artefacts/venv/bin/python
export PA_HALF=1 PA_EVS=-2,-3 PA_OUT=pair_align-flat27.json SSIM_METHODS=opposed,c-def,c-p11r,c-p11lr,c-k065,c-k065p OMP_NUM_THREADS=3
sed 's#^#runs/#' scratch/included.txt | xargs -P 4 -n 1 nice -n 19 $P scripts/pair_align.py > $T/score3.log 2>&1
for d in $(sed 's#^#runs/#' scratch/included.txt); do rm -f $d/bl-2_*.png $d/bl-3_*.png; done
D=$(sed 's#^#runs/#' scratch/included.txt)
for m in c-k065 c-k065p; do
  PT_IN=pair_align-flat27.json PT_EVS=-2,-3 PT_METHODS=c-def,$m $P scripts/pair_table.py $D > $T/table3-$m.txt
done
# against passes 11 + per region + from rim (no fill, specks 0)
for m in c-k065 c-k065p; do
  PT_IN=pair_align-flat27.json PT_EVS=-2,-3 PT_METHODS=c-p11lr,$m $P scripts/pair_table.py $D > $T/table3-perregion-vs-$m.txt
done
echo done

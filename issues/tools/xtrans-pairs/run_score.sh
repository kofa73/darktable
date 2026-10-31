#!/bin/bash
# pair_align.py at -2 / -3 EV on the X-Trans pairs (opposed first), tables against inpaint opposed
cd /tmp/hl-rules-study
T=tasks/2026-10-31-xtrans
P=/tmp/claude-gd-artefacts/venv/bin/python
D=$(ls -d runs/xtrans/x*)
export PA_HALF=1 PA_EVS=-2,-3 PA_OUT=pair_align-xt.json SSIM_METHODS=opposed,c-k065,c-k065p,c-p11r OMP_NUM_THREADS=3
echo $D | xargs -P 4 -n 1 nice -n 19 $P scripts/pair_align.py > $T/score.log 2>&1
for m in c-k065 c-k065p c-p11r; do
  PT_IN=pair_align-xt.json PT_EVS=-2,-3 PT_METHODS=opposed,$m $P scripts/pair_table.py $D > $T/table-$m.txt
done
# reference passes 11 added, and per region + flat lift added
PT_IN=pair_align-xt.json PT_EVS=-2,-3 PT_METHODS=c-k065,c-k065p $P scripts/pair_table.py $D > $T/table-passes.txt
PT_IN=pair_align-xt.json PT_EVS=-2,-3 PT_METHODS=c-p11r,c-k065p $P scripts/pair_table.py $D > $T/table-perregion.txt
echo finished

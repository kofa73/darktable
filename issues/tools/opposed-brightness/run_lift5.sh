#!/bin/bash
# issue 29: domes with the enclosed clipped specks merged (LIFT_HOLES) and the smoothed distance
cd /tmp/hl-rules-study
T=tasks/2026-10-24-brightness
P=/tmp/claude-gd-artefacts/venv/bin/python
export OMP_NUM_THREADS=3 OPENBLAS_NUM_THREADS=3 MKL_NUM_THREADS=3 LIFT_ROBUST=0.8 LIFT_SOFT=2 LIFT_HOLES=1
v() { # name order log dsmooth src list
  LIFT_OUT=$1 LIFT_ORDER=$2 LIFT_LOG=$3 LIFT_DSMOOTH=$4 LIFT_SRC=$5 xargs -P 4 -n 4 nice -n 19 $P tasks/2026-10-24-lift-ideas/lift2.py < $6 > $T/log-$1.txt 2>&1
}
v opp-hdomelin 4 0 1 opposed $T/dirs.txt
v opp-hdomelog 4 1 1 opposed $T/dirs.txt
v opp-rhdomelin 2 0 0 opposed $T/dirs.txt
v opl-hdomelin 4 0 1 opp-local $T/dirs-local.txt
echo done

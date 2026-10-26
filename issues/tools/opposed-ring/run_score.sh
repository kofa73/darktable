#!/bin/bash
# issue 30: pair_align.py at -2 / -3 EV on the 103 scored pairs; $1 = tag, $2 = methods (opposed first)
cd /tmp/hl-rules-study
P=/tmp/claude-gd-artefacts/venv/bin/python
export PA_HALF=1 PA_EVS=-2,-3 PA_OUT=pair_align-$1.json SSIM_METHODS=$2 OMP_NUM_THREADS=3
sed 's#^#runs/#' scratch/included.txt | xargs -P 4 -n 1 nice -n 19 $P scripts/pair_align.py > tasks/2026-10-25-ring/score-$1.log 2>&1
# the saved panels are only needed for blend_sheet.py; drop them
for d in $(sed 's#^#runs/#' scratch/included.txt); do rm -f $d/bl-2_*.png $d/bl-3_*.png; done
echo done

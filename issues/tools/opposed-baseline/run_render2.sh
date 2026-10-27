#!/bin/bash
# renders the "from rim" variants without the cube root into the 103 scored workdirs (4 workers)
cd /tmp/hl-rules-study
T=tasks/2026-10-27-baseline
P=/tmp/claude-gd-artefacts/venv/bin/python
export R_METHODS=c-p11r,c-p11lr,c-p11r-d1,c-p11r-d2 EV2_THREADS=3 OMP_NUM_THREADS=3 OMP_WAIT_POLICY=passive
sed 's#^#runs/#' scratch/included.txt > $T/dirs.txt
split -n r/4 -d $T/dirs.txt $T/part
for i in 0 1 2 3; do
  R_CFG=$T/cfg$i nice -n 19 xargs -a $T/part0$i $P $T/render.py > $T/renderB$i.log 2>&1 &
done
wait
echo done

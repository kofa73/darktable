#!/bin/bash
# renders c-def, c-best, c-nodome into the 103 scored workdirs (4 workers, one config each)
cd /tmp/hl-rules-study
T=tasks/2026-10-27-baseline
P=/tmp/claude-gd-artefacts/venv/bin/python
export R_METHODS=c-def,c-best,c-nodome EV2_THREADS=3 OMP_NUM_THREADS=3 OMP_WAIT_POLICY=passive
sed 's#^#runs/#' scratch/included.txt > $T/dirs.txt
split -n r/4 -d $T/dirs.txt $T/part
for i in 0 1 2 3; do
  R_CFG=$T/cfg$i nice -n 19 xargs -a $T/part0$i $P $T/render.py > $T/render$i.log 2>&1 &
done
wait
echo done

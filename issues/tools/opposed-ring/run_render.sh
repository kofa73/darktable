#!/bin/bash
# issue 30: renders of the opposed variants into the 103 scored workdirs (4 workers, one config each)
cd /tmp/hl-rules-study
P=/tmp/claude-gd-artefacts/venv/bin/python
export RX_METHODS=$1 EV2_THREADS=3 OMP_NUM_THREADS=3 OMP_WAIT_POLICY=passive
sed 's#^#runs/#' scratch/included.txt > /tmp/hl-rules-study/tasks/2026-10-25-ring/dirs.txt
split -n r/4 -d tasks/2026-10-25-ring/dirs.txt tasks/2026-10-25-ring/part
for i in 0 1 2 3; do
  SYNTH_CFG=runs/allpairs/cfg$i nice -n 19 xargs -a tasks/2026-10-25-ring/part0$i $P scripts/render_extra.py > tasks/2026-10-25-ring/render$i.log 2>&1 &
done
wait
echo done

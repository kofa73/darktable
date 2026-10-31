#!/bin/bash
# renders the X-Trans pairs of plan.json (4 workers, 3 threads each) with the opposed-controls build
cd /tmp/hl-rules-study/tasks/2026-10-31-xtrans
P=/tmp/claude-gd-artefacts/venv/bin/python
export EV2_THREADS=3 OMP_NUM_THREADS=3 OMP_WAIT_POLICY=passive
$P - > jobs.txt <<'PY'
import json
for e in json.load(open('plan.json')):
    print(e['ref'], e['clipped'], e['k'], '/tmp/hl-rules-study/runs/xtrans/' + e['name'])
PY
split -n r/4 -d jobs.txt part
for i in 0 1 2 3; do
  ( while read -r ref clp k wd; do
      [ -f "$wd/scores.json" ] && continue
      mkdir -p "$wd"
      SYNTH_CFG=cfg$i nice -n 19 $P xt_ev2.py "$ref" "$clp" "$k" "$wd" none opposed c-k065 c-k065p c-p11r > "$wd/log.txt" 2>&1 \
        && echo "done $(basename $wd)" || echo "FAILED $(basename $wd)"
    done < part0$i ) > render$i.log 2>&1 &
done
wait
echo finished

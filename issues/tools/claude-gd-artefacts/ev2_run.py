# ev2_run.py <plan.json> <outdir> <workers> [methods...]
# Runs ev2.py for every entry of the plan, in parallel workers with their own copy of
# the darktable config directory. Entries with a scores.json are skipped. Prints "done" or
# "FAILED" (with the last line of the entry's log.txt) per entry, and exits 1 if any failed.
# Entry: {"name", "kind": "synth"|"pair", "ref", "clipped", "k"}; k is the synthetic
# EV for synth and the measured exposure difference for pairs.
import sys, os, json, shutil, subprocess
from concurrent.futures import ThreadPoolExecutor
plan, outdir, nw = json.load(open(sys.argv[1])), sys.argv[2], int(sys.argv[3])
methods = sys.argv[4:]
os.makedirs(outdir, exist_ok=True)

FAILED = []


def run(e, cfg):
    wd = os.path.join(outdir, e['name'])
    if os.path.exists(os.path.join(wd, 'scores.json')):
        return
    os.makedirs(wd, exist_ok=True)
    args = [e['ref'], str(e['k'])] if e['kind'] == 'synth' else [e['ref'], e['clipped'], str(e['k'])]
    with open(os.path.join(wd, 'log.txt'), 'w') as log:
        r = subprocess.run(['/tmp/claude-gd-artefacts/venv/bin/python', '/tmp/claude-gd-artefacts/scripts/ev2.py',
                            e['kind']] + args + [wd] + methods, env=dict(os.environ, SYNTH_CFG=cfg),
                           stdout=log, stderr=subprocess.STDOUT)
    # ev2 exits non-zero and writes no scores.json when any render fails
    if r.returncode or not os.path.exists(os.path.join(wd, 'scores.json')):
        last = [l for l in open(os.path.join(wd, 'log.txt')).read().splitlines() if l.strip()]
        FAILED.append(e['name'])
        print('FAILED', e['name'], '(exit %d):' % r.returncode, last[-1] if last else '', flush=True)
    else:
        print('done', e['name'], flush=True)


for w in range(nw):
    cfg = os.path.join(outdir, 'cfg%d' % w)
    if not os.path.exists(cfg):
        shutil.copytree('/tmp/hlc-eval/cfg', cfg, ignore=shutil.ignore_patterns('*.lock', 'library.db*'))
# jobs of one worker share its config directory, so each worker takes every nw-th job
with ThreadPoolExecutor(nw) as ex:
    futs = [ex.submit(lambda w=w: [run(e, os.path.join(outdir, 'cfg%d' % w)) for i, e in enumerate(plan) if i % nw == w])
            for w in range(nw)]
    for f in futs:
        f.result()
if FAILED:
    print('%d of %d failed:' % (len(FAILED), len(plan)), ' '.join(FAILED), flush=True)
    sys.exit(1)

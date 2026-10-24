# synth_run.py <plan.json> <outdir> <workers> [methods...]
# Runs synth_eval.py for every image of the plan, in parallel workers with their own
# copy of the darktable config directory. Images with a scores.json are skipped.
import sys, os, json, shutil, subprocess
from concurrent.futures import ThreadPoolExecutor
plan, outdir, nw = json.load(open(sys.argv[1])), sys.argv[2], int(sys.argv[3])
methods = sys.argv[4:]
os.makedirs(outdir, exist_ok=True)


def run(job):
    i, e = job
    wd = os.path.join(outdir, e['dir'].replace('/', '_'))
    if os.path.exists(os.path.join(wd, 'scores.json')):
        return
    cfg = os.path.join(outdir, 'cfg%d' % (i % nw))
    env = dict(os.environ, SYNTH_CFG=cfg)
    os.makedirs(wd, exist_ok=True)
    with open(os.path.join(wd, 'log.txt'), 'w') as log:
        subprocess.run(['/tmp/claude-gd-artefacts/venv/bin/python', '/tmp/claude-gd-artefacts/scripts/synth_eval.py',
                        e['raw'], str(e['k']), wd] + methods, env=env, stdout=log, stderr=subprocess.STDOUT)
    print('done', e['dir'], flush=True)


for w in range(nw):
    cfg = os.path.join(outdir, 'cfg%d' % w)
    if not os.path.exists(cfg):
        shutil.copytree('/tmp/hlc-eval/cfg', cfg, ignore=shutil.ignore_patterns('*.lock', 'library.db*'))
# jobs of one worker share its config directory, so each worker takes every nw-th job
with ThreadPoolExecutor(nw) as ex:
    futs = [ex.submit(lambda w=w: [run((i, e)) for i, e in enumerate(plan) if i % nw == w]) for w in range(nw)]
    for f in futs:
        f.result()

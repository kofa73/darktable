# synth_summary.py <run dir> [subset]
# Summarizes the scores.json files of synth_eval.py or pair_eval.py runs: for each
# method the mean over images of the per-image mean dE2000 and mean chroma error in
# the scored pixels, the number of images where it has the lowest dE, and the same
# per category (the part of the directory name before the first underscore).
import sys, os, json, glob, collections, numpy as np
run = sys.argv[1]
subset = sys.argv[2] if len(sys.argv) > 2 else 'all'
S = {}
for f in sorted(glob.glob(os.path.join(run, '*', 'scores.json'))):
    S[os.path.basename(os.path.dirname(f))] = json.load(open(f))
methods = []
for s in S.values():
    for m in s['methods']:
        if m not in methods:
            methods.append(m)


def table(names, title):
    rows = []
    best = collections.Counter()
    for n in names:
        ms = {m: S[n]['methods'][m][subset] for m in methods if subset in S[n]['methods'].get(m, {})}
        cand = {m: v['dE_mean'] for m, v in ms.items() if m != 'none'}
        if cand:
            best[min(cand, key=cand.get)] += 1
    used = [n for n in names if all(subset in S[n]['methods'].get(m, {}) for m in methods)]
    print('%s: %d images with "%s" pixels in every method (%d with some)' % (title, len(used), subset, len(names)))
    print('  %-10s %8s %8s %8s %8s %6s' % ('method', 'dE mean', 'dE p90', 'dab', 'dL', 'best'))
    for m in methods:
        v = [S[n]['methods'][m][subset] for n in used]
        if not v:
            continue
        print('  %-10s %8.2f %8.2f %8.2f %8.2f %6d' % (m, np.mean([x['dE_mean'] for x in v]), np.mean([x['dE_p90'] for x in v]),
              np.mean([x['dab_mean'] for x in v]), np.mean([x['dL_mean'] for x in v]), best[m]))


table(list(S), 'all images')
cats = collections.defaultdict(list)
for n in S:
    cats[n.split('_')[0]].append(n)
if len(cats) > 1:
    for c, names in sorted(cats.items()):
        table(names, c)

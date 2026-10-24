# seg_oracle.py <workdir>...: issue segmentation-auto-settings. Reads pair_align-seg.json (PT_IN to
# change) and, per pair, takes the best of the segmentation based settings (sum of SSIMULACRA 2 at 0
# and -1 EV) as a per-image oracle. Prints the oracle's gain over the defaults (segments) per pair
# and scene type, how often each setting is best, and the oracle against inpaint opposed and gradient
# domain. Excludes pairs as pair_table.py does (motion > 15%, truth < 50%).
import sys, os, json
import numpy as np
from collections import defaultdict, Counter

IN = os.environ.get('PT_IN', 'pair_align-seg.json')
SETS = ['segments'] + ['seg-c%d-k%d' % (c, k) for c in (1, 2, 4) for k in (2, 5, 8)]
rows = []
for wd in sys.argv[1:]:
    r = json.load(open(os.path.join(wd, IN)))
    if (r['motion'] or 0) > 0.15 or r['truth_share'] < 0.5:
        continue
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    kind = sc['ref'].split('/bracketed/')[1].split('/')[0]
    tot = {m: (r[m + ' +0'], r[m + ' -1']) for m in SETS + ['opposed', 'gd-wb4x10']}
    best = max(SETS, key=lambda m: sum(tot[m]))
    rows.append((kind, os.path.basename(wd.rstrip('/')), best, tot))

print('per-image oracle over segmentation based settings, gain over the defaults (0 / -1 EV)')
by = defaultdict(list)
for r in rows:
    by[r[0]].append(r)
    by['all'].append(r)
for k in sorted(by):
    g = np.array([[r[3][r[2]][e] - r[3]['segments'][e] for e in (0, 1)] for r in by[k]])
    vo = np.array([[r[3][r[2]][e] - r[3]['opposed'][e] for e in (0, 1)] for r in by[k]])
    vg = np.array([[r[3][r[2]][e] - r[3]['gd-wb4x10'][e] for e in (0, 1)] for r in by[k]])
    print('%-9s n %3d  gain median %+5.1f/%+5.1f mean %+5.1f/%+5.1f  >=2 in %2d pairs | oracle-opposed median %+5.1f/%+5.1f'
          ' | oracle-gd median %+5.1f/%+5.1f' % (k, len(g), *np.median(g, 0), *g.mean(0), int((g.max(1) >= 2).sum()),
                                                 *np.median(vo, 0), *np.median(vg, 0)))
print()
print('best setting counts:', ', '.join('%s %d' % kv for kv in Counter(r[2] for r in rows).most_common()))
print()
print('pairs with oracle gain >= 2 (0 / -1 EV), best setting, defaults, opposed, gradient domain:')
for r in sorted(rows, key=lambda r: -sum(r[3][r[2]]) + sum(r[3]['segments'])):
    g = [r[3][r[2]][e] - r[3]['segments'][e] for e in (0, 1)]
    if max(g) >= 2:
        print('  %-9s %-44s %+5.1f %+5.1f  %-12s def %5.1f/%5.1f  opp %5.1f/%5.1f  gd %5.1f/%5.1f' % (
            r[0], r[1][:44], *g, r[2], *r[3]['segments'], *r[3]['opposed'], *r[3]['gd-wb4x10']))

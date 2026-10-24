# hanno_compare.py <run dir>[,<run dir>...] [out.txt]: scores of several runs merged per image; each variant against its own master counterpart
# (inpaint opposed, segmentation based, segmentation based with rebuild), per mask: images
# better / worse by >= 2 dE and >= 20% (better) / 25% (worse), the mean dE, and the same on
# the failing images (best of master inpaint opposed / segmentation based > dE 10)
import json, glob, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rescue2 import fixed, broke

PAIRS = [('opposed', 'opp-cons'), ('opposed', 'opp-asshot'), ('segments', 'seg-thresh'), ('segments', 'seg-full'),
         ('segments', 'seg-asshot'), ('segments', 'segrb'), ('segrb', 'segrb-thresh'), ('segrb', 'segrb-thresh-cbrt'),
         ('segrb-thresh', 'segrb-thresh-cbrt'), ('segrb', 'segrb-full-cbrt'), ('segrb', 'segrb-asshot-cbrt')]
UI = {'opposed': 'inpaint opposed', 'segments': 'segmentation based', 'segrb': 'segm. based + rebuild',
      'opp-cons': 'opposed, D65-consistent', 'opp-asshot': 'opposed, as-shot-consistent',
      'seg-thresh': 'segm., threshold fix', 'seg-full': 'segm., D65-consistent',
      'seg-asshot': 'segm., as-shot-consistent', 'segrb-thresh': 'segm.+rb, threshold fix',
      'segrb-thresh-cbrt': 'segm.+rb, threshold + plane fix', 'segrb-full-cbrt': 'segm.+rb, D65-consistent + plane',
      'segrb-asshot-cbrt': 'segm.+rb, as-shot-consistent + plane'}
S = {}
runs = sys.argv[1].split(',')
names = {os.path.basename(os.path.dirname(f)) for f in glob.glob(os.path.join(runs[-1], '*', 'scores.json'))}
for run in runs:
    for f in glob.glob(os.path.join(run, '*', 'scores.json')):
        n = os.path.basename(os.path.dirname(f))
        if n in names:   # only the images of the last run
            S.setdefault(n, {}).update(json.load(open(f))['methods'])
lines = ['%d images' % len(S)]
for mk in ('all', 'partial', 'full'):
    lines.append('\n== mask %s' % mk)
    lines.append('%-44s %-40s %8s %8s %12s | %s' % ('variant', 'against', 'better', 'worse', 'mean a -> b',
                                                   'failing: better/worse of n, mean'))
    for a, b in PAIRS:
        rows = [(n, v[a][mk]['dE_mean'], v[b][mk]['dE_mean'],
                 min(v['opposed'][mk]['dE_mean'], v['segments'][mk]['dE_mean']))
                for n, v in S.items() if a in v and b in v and mk in v[a] and mk in v[b]]
        if not rows:
            continue
        A = np.array([r[1] for r in rows]); B = np.array([r[2] for r in rows]); O = np.array([r[3] for r in rows])
        bt = sum(fixed(x, y) for x, y in zip(A, B)); wo = sum(broke(x, y) for x, y in zip(A, B))
        f = O > 10
        fb = sum(fixed(x, y) for x, y in zip(A[f], B[f])); fw = sum(broke(x, y) for x, y in zip(A[f], B[f]))
        lines.append('%-44s %-40s %8d %8d %5.2f->%5.2f | %d/%d of %d, %.2f->%.2f' % (
            UI[b], UI[a], bt, wo, A.mean(), B.mean(), fb, fw, f.sum(), A[f].mean(), B[f].mean()))
    # largest changes
    for a, b in PAIRS:
        d = sorted(((v[b][mk]['dE_mean'] - v[a][mk]['dE_mean'], n, v[a][mk]['dE_mean'], v[b][mk]['dE_mean'])
                    for n, v in S.items() if a in v and b in v and mk in v[a] and mk in v[b]))
        if d and (abs(d[0][0]) >= 1 or abs(d[-1][0]) >= 1):
            lines.append('   %s vs %s: best %s; worst %s' % (b, a, ', '.join('%s %.1f->%.1f' % x[1:] for x in d[:3] if x[0] < -1),
                                                            ', '.join('%s %.1f->%.1f' % x[1:] for x in d[::-1][:3] if x[0] > 1)))
out = '\n'.join(lines)
print(out)
if len(sys.argv) > 2:
    open(sys.argv[2], 'w').write(out + '\n')

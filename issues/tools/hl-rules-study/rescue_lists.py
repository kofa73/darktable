# rescue_lists.py <run dir> <out.md>: for the masks all / partly / fully clipped, the images
# where the better of inpaint opposed and segmentation based scores above dE 10, split into
# rescued / made worse / unchanged by "continuous border, hue estimate in fully clipped",
# and those rescued by the runner-up of each mask; with the clipped raw file and the scores
import json, glob, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rescue2 import fixed, broke

MAIN = ('gd-wb4x10', 'continuous border, hue estimate in fully clipped')
NEXT = {'all': ('gd-wb4x7', 'hue from measured channels, hue estimate in fully clipped'),
        'full': ('gd-wb4x7', 'hue from measured channels, hue estimate in fully clipped'),
        'partial': ('gd-wb4x11', 'continuous border, white in fully clipped')}
LABEL = {'all': 'all clipped pixels', 'partial': 'partly clipped', 'full': 'fully clipped'}
run, outp = sys.argv[1], sys.argv[2]
S = {}
for f in sorted(glob.glob(os.path.join(run, '*', 'scores.json'))):
    s = json.load(open(f))
    S[os.path.basename(os.path.dirname(f))] = s
out = []
for mk in ('all', 'full', 'partial'):
    rows = []
    for n, s in S.items():
        m = s['methods']
        if any(mk not in m.get(k, {}) for k in ('opposed', 'segments', MAIN[0], NEXT[mk][0])):
            continue
        o = min(m['opposed'][mk]['dE_mean'], m['segments'][mk]['dE_mean'])
        if o <= 10:
            continue
        g, x = m[MAIN[0]][mk]['dE_mean'], m[NEXT[mk][0]][mk]['dE_mean']
        st = 'rescued' if fixed(o, g) else 'worse' if broke(o, g) else 'unchanged'
        rows.append((n, os.path.basename(s['clipped']), o, g, x, st, fixed(o, x)))
    out.append('## %s: %d images where inpaint opposed and segmentation based fail (dE > 10)\n' % (LABEL[mk], len(rows)))
    out.append('Columns: image, clipped raw, best of opposed/segmentation, "%s", "%s" (runner-up), runner-up rescues.\n'
               % (MAIN[1], NEXT[mk][1]))
    for st in ('rescued', 'worse', 'unchanged'):
        sel = sorted([r for r in rows if r[5] == st], key=lambda r: r[0])
        out.append('### %s by "%s": %d\n' % (st, MAIN[1], len(sel)))
        out.append('| image | clipped raw | opposed/segm. | %s | runner-up | runner-up rescues |' % MAIN[0].replace('gd-wb4x', 'value '))
        out.append('|---|---|---|---|---|---|')
        for r in sel:
            out.append('| %s | %s | %.1f | %.1f | %.1f | %s |' % (r[0], r[1], r[2], r[3], r[4], 'yes' if r[6] else ''))
        out.append('')
open(outp, 'w').write('\n'.join(out) + '\n')

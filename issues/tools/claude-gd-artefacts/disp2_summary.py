# disp2_summary.py <plan.json> <run>[+<run>...]... : display scores of the gradient domain variants
# against the better of opposed and segments ("def") per image, and totals per run.
# Per cell: mean dE2000 on all clipped pixels / patches above dE 5 (count) / local
# contrast in deep pixels relative to the truth.
import json, os, sys, numpy as np
plan = json.load(open(sys.argv[1])); runs = sys.argv[2:]
V = [v for v in os.environ.get('VARIANTS', 'gd-wb4x7,gd-wb4x8,gd-wb4x10,gd-wb4x11,gd-wb4x12').split(',')]
NAMES = {'gd-wb4x7': 'measured+hue', 'gd-wb4x8': 'measured+white', 'gd-wb4x10': 'contin.+hue',
         'gd-wb4x11': 'contin.+white', 'gd-wb4x12': 'opposed+white', 'gd-wb4x9': 'opposed+hue'}
def cell(m):
    a = m['all']; d = m.get('deep', {})
    return '%4.1f/%2d/%s' % (a['dE_mean'], m['patches']['count'], ('%.2f' % d['contrast']) if 'contrast' in d else '  - ')
for r in runs:
    print('== %s' % r)
    print('%-24s %4s  %-14s %s' % ('image', 'clip', 'default', '  '.join('%-14s' % NAMES[v] for v in V)))
    tot = {v: [0, 0, 0, 0] for v in V}; con = {v: [] for v in V + ['def']}
    for e in plan:
        # runs joined by + share the truth, masks and exposure: their methods merge
        fs = [os.path.join(x, e['name'], 'scores.json') for x in r.split('+')]
        if not all(os.path.exists(f) for f in fs):
            continue
        s = json.load(open(fs[0])); M = dict(s['methods'])
        for f in fs[1:]:
            M.update(json.load(open(f))['methods'])
        if any(v not in M for v in V):
            continue
        dk = min(('opposed', 'segments'), key=lambda k: M[k]['all']['dE_mean'])
        D = M[dk]
        print('%-24s %4.1f  %-14s %s' % (e['name'][:24], s['display']['clip_above_grey_ev'], cell(D),
                                         '  '.join('%-14s' % cell(M[v]) for v in V)))
        if 'contrast' in D.get('deep', {}):
            con['def'].append(D['deep']['contrast'])
        for v in V:
            dd = M[v]['all']['dE_mean'] - D['all']['dE_mean']
            tot[v][0] += dd <= -1; tot[v][1] += dd >= 1
            tot[v][2] += M[v]['patches']['count'] > D['patches']['count']
            tot[v][3] += M[v]['patches']['largest'] > 0.05
            if 'contrast' in M[v].get('deep', {}):
                con[v].append(M[v]['deep']['contrast'])
    print('  totals: better by >=1 dE / worse by >=1 dE / more patches than default / a patch over 5% of the region; deep contrast median')
    print('  %-14s %s' % ('default', '%.2f' % np.median(con['def']) if con['def'] else '-'))
    for v in V:
        print('  %-14s %2d / %2d / %2d / %2d;  %.2f' % (NAMES[v], *tot[v], np.median(con[v]) if con[v] else float('nan')))

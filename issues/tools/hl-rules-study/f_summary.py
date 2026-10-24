# f_summary.py <f_rule.json> <groups.json>: Phase 3 summary. Per group (sky, colored, the
# other synthetic images) and mask, the mean over images of the angular error (degrees,
# metric rho and implied, see f_rule.py) of each estimate, and per image the change of
# each F variant against its base (better / worse by more than 0.5 degrees).
import sys, json
import numpy as np
R = json.load(open(sys.argv[1]))
G = json.load(open(sys.argv[2]))
groups = {'sky': G['sky'], 'colored': G['colored'],
          'other synthetic': [n for n in R if n.startswith('syn_') and n not in G['sky'] + G['colored']]}
EST = ['measured (7)', 'continuous (10)', "opposed's (9)", 'F everywhere', 'F gated on 10', 'F gated on 7',
       'F gated on 7, 2+ flags', 'F gated on 10, 2+ flags']
BASE = {'F everywhere': 'continuous (10)', 'F gated on 10': 'continuous (10)', 'F gated on 7': 'measured (7)',
        'F gated on 7, 2+ flags': 'measured (7)', 'F gated on 10, 2+ flags': 'continuous (10)'}
for metric in ('implied', 'rho'):
    print('\n######## metric', metric)
    for g, names in groups.items():
        names = [n for n in names if n in R]
        for mk in ('partial', 'full'):
            have = [n for n in names if '%s|%s|%s' % (metric, mk, EST[0]) in R[n]]
            if not have:
                continue
            print('\n%s, %s clipped (%d images)' % (g, mk, len(have)))
            for e in EST:
                v = np.array([R[n]['%s|%s|%s' % (metric, mk, e)] for n in have])
                line = '  %-26s mean %6.2f  median %6.2f' % (e, v.mean(), np.median(v))
                if e in BASE:
                    b = np.array([R[n]['%s|%s|%s' % (metric, mk, BASE[e])] for n in have])
                    line += '   vs %-16s better %2d worse %2d' % (BASE[e], (v < b - 0.5).sum(), (v > b + 0.5).sum())
                print(line)
            if metric == 'implied' and mk == 'full':
                for n in have:
                    print('    %-28s ' % n[:28] + ' '.join('%5.1f' % R[n]['%s|%s|%s' % (metric, mk, e)] for e in EST)
                          + '  gate %.2f' % R[n]['gate_share'])

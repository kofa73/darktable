# ev2_summary.py <run dir> [<run dir>...] [-m mask,mask] [-g]
# Summarizes ev2.py results. Several run directories can hold different methods for
# the same images; they are merged per image name. For each mask: per method the
# mean over images of dE (tone mapped), duv x 1000, |log2 Y ratio|, and, paired with
# opposed on the images where both have the mask, the mean difference in dE and the
# number of images where the method is better / worse by more than 0.5 dE.
# -g adds the same per image group (the name prefix before the first underscore).
import sys, os, json, glob, collections, numpy as np
args = sys.argv[1:]
masks = ['all', 'partial', 'full', 'deep', 'colored', 'rest']
group = '-g' in args
if '-m' in args:
    masks = args[args.index('-m') + 1].split(',')
runs = [a for i, a in enumerate(args) if not a.startswith('-') and (i == 0 or args[i - 1] != '-m')]
S = collections.defaultdict(dict)
for r in runs:
    for f in sorted(glob.glob(os.path.join(r, '*', 'scores.json'))):
        name = os.path.basename(os.path.dirname(f))
        for m, v in json.load(open(f))['methods'].items():
            S[name].setdefault(m, v)
methods = []
for v in S.values():
    for m in v:
        if m not in methods:
            methods.append(m)


def table(names, title):
    print('\n== %s (%d images)' % (title, len(names)))
    for mk in masks:
        have = [n for n in names if mk in S[n].get('opposed', {})]
        if not have:
            continue
        print('  mask %-8s %d images' % (mk, len(have)))
        print('    %-10s %6s %6s %6s %5s | vs opposed: %6s %4s %4s' % ('method', 'dE', 'duv', 'dlogY', 'n', 'ddE', 'win', 'loss'))
        for m in methods:
            ns = [n for n in have if mk in S[n].get(m, {})]
            if not ns:
                continue
            v = [S[n][m][mk] for n in ns]
            d = np.array([S[n][m][mk]['dE_mean'] - S[n]['opposed'][mk]['dE_mean'] for n in ns])
            print('    %-10s %6.2f %6.2f %6.3f %5d | %+10.2f %4d %4d' % (
                m, np.mean([x['dE_mean'] for x in v]), 1000 * np.mean([x['duv_mean'] for x in v]),
                np.mean([x['dlogY_abs'] for x in v]), len(ns), d.mean(), (d < -0.5).sum(), (d > 0.5).sum()))


table(sorted(S), 'all images')
if group:
    g = collections.defaultdict(list)
    for n in sorted(S):
        g[n.split('_')[0]].append(n)
    for k, names in g.items():
        table(names, k)

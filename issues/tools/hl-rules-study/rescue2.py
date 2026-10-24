# rescue2.py <run dir>[,<run dir>...] <mask> [threshold] [-v variants] [-l]
# rescue.py of /tmp/claude-gd-artefacts/scripts with the run directories and variants as
# arguments. The failing set: images where the better of inpaint opposed and
# segmentation based ("others") scores above the threshold (default 10) on the mask.
# fixed: >= 2 dE and >= 20% better than others; broke: >= 2 dE and >= 25% worse, counted
# separately on the failing images ("worse") and on the images where the defaults are
# fine ("broke(ok)"). -l lists the failing images.
import json, glob, os, sys
import numpy as np

UI = {'gd-wb4x7': 'measured, hue fill', 'gd-wb4x8': 'measured, white fill', 'gd-wb4x13': 'measured, edge fill',
      'gd-wb4x10': 'continuous, hue fill', 'gd-wb4x11': 'continuous, white fill', 'gd-wb4x14': 'continuous, edge fill',
      'gd-wb4x9': "opposed's est., hue fill"}


def fixed(o, g):
    return g <= o - 2 and g <= 0.8 * o


def broke(o, g):
    return g >= o + 2 and g >= 1.25 * o


def load(runs):
    S = {}
    for run in runs:
        for f in glob.glob(os.path.join(run, '*', 'scores.json')):
            n = os.path.basename(os.path.dirname(f))
            S.setdefault(n, {}).update(json.load(open(f))['methods'])
    return S


def count(S, mk, T, G, names=None):
    rows = []
    for n, v in S.items():
        if names is not None and n not in names:
            continue
        if mk not in v.get('opposed', {}) or any(g not in v or mk not in v[g] for g in G):
            continue
        o = min(v['opposed'][mk]['dE_mean'], v['segments'][mk]['dE_mean'])
        rows.append((n, o, {g: v[g][mk]['dE_mean'] for g in G}))
    return rows


def main():
    a = sys.argv[1:]
    lst = '-l' in a
    G = list(UI)
    if '-v' in a:
        G = a[a.index('-v') + 1].split(',')
    pos = [x for i, x in enumerate(a) if not x.startswith('-') and (i == 0 or a[i - 1] != '-v')]
    runs, mk = pos[0].split(','), pos[1]
    T = float(pos[2]) if len(pos) > 2 else 10.0
    S = load(runs)
    rows = count(S, mk, T, G)
    fail = [r for r in rows if r[1] > T]
    ok = [r for r in rows if r[1] <= T]
    print('mask %s: %d images, %d where the better of inpaint opposed / segmentation based has dE > %g'
          % (mk, len(rows), len(fail), T))
    print('%-28s %6s %6s %10s %8s' % ('variant', 'fixed', 'worse', 'broke(ok)', 'mean'))
    for g in G:
        print('%-28s %6d %6d %6d/%-3d %8.2f' % (UI.get(g, g), sum(fixed(o, d[g]) for _, o, d in fail),
                                                sum(broke(o, d[g]) for _, o, d in fail),
                                                sum(broke(o, d[g]) for _, o, d in ok), len(ok),
                                                np.mean([d[g] for _, _, d in rows])))
    anyfix = [r for r in fail if any(fixed(r[1], r[2][g]) for g in G)]
    print('fixed by at least one variant: %d of %d' % (len(anyfix), len(fail)))
    src = lambda n: n.split('_')[0]
    for s in sorted(set(src(r[0]) for r in rows)):
        f = [r for r in fail if src(r[0]) == s]
        print('  %-5s failing %3d of %3d; fixed: %s' % (s, len(f), sum(src(r[0]) == s for r in rows),
                                                       ' '.join('%d' % sum(fixed(o, d[g]) for _, o, d in f) for g in G)))
    if lst:
        for n, o, d in sorted(fail, key=lambda r: -r[1]):
            print('  %-28s %5.1f  ' % (n[:28], o) + ' '.join('%5.1f%s' % (d[g], '*' if fixed(o, d[g]) else ('!' if broke(o, d[g]) else ' ')) for g in G))


if __name__ == '__main__':
    main()

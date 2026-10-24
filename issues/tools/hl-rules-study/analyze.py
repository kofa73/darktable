# analyze.py <run dir> [set] [-o out.json]: Phase 2 of the rules study, from the
# regions.json and sums.npz that regions.py writes per image.
#
# 1. Predictive power. Per feature and hue source (set: model = "hue from measured
#    channels, ...", cont = "continuous border, ..."), for the region labels
#    "white better than hue" (dE_white < dE_hue) and "edge better than hue": the weighted
#    AUC (probability that a random "better" region has the larger feature value, pairs
#    weighted by the product of the region weights) and the weighted accuracy of the best
#    single threshold. Weights: the region's scored pixels (size), and size x |dE
#    difference| (stake: the dE that the choice moves). Per source and for all.
# 2. Selector simulation. A rule is (feature, threshold, side, fill above, fill below,
#    fallback): regions of >= 50 scored pixels whose feature is finite take the fill of
#    their side; the others, and every zone too small to be in the table, take the
#    fallback. The composite's dE per mask is exact for per-zone selection (see
#    regions.py). Thresholds are tuned on one half and scored on the other: split by
#    source, {si, syn} against {fc, iain}, both directions. Objective on the tuning half:
#    rescues on the fully clipped mask, fixed minus broken among the images where the
#    defaults are fine, ties broken by the mean dE of the fully clipped mask.
# 3. Oracle: the per-region best fill by the truth, an upper bound for any selector.
import sys, os, json, glob, itertools
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rescue2 import fixed, broke

FILLS = ('hue', 'white', 'edge')
SETS = {'model': ('gd-wb4x7', 'gd-wb4x8', 'gd-wb4x13'), 'cont': ('gd-wb4x10', 'gd-wb4x11', 'gd-wb4x14')}
MASKS = ('all', 'partial', 'full')
FEATS = ['A1', 'A2', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6', 'B7', 'C1', 'C2', 'C1r', 'C2r', 'D1', 'D2']  # C1r, C2r: of the set's own render
THR = 10.0
HALVES = {'si+syn': ('si', 'syn'), 'fc+iain': ('fc', 'iain')}


def load(run, st):
    imgs = []
    for f in sorted(glob.glob(os.path.join(run, '*', 'regions.json'))):
        wd = os.path.dirname(f)
        d = json.load(open(f))
        if not d.get('nzones'):
            continue
        s = np.load(os.path.join(wd, 'sums.npz'))
        ms = SETS[st]
        im = dict(name=d['name'], source=d['source'], others={m: min(d['others'][m].values()) for m in MASKS
                                                              if d['others'].get(m)},
                  regions=d['regions'], nz=d['nzones'], set=st)
        # per mask: n total, S0 (outside zones, all fills equal) and S[r, fill]
        for m in MASKS:
            n = s['n|' + m]
            S = np.stack([s['dE|%s|%s' % (m, x)] for x in ms], 1)
            im['n_' + m] = float(n.sum())
            im['S_' + m] = S
            im['n_%s_z' % m] = n
        imgs.append(im)
    return imgs


def feat(r, fn, st):
    """Feature value of region r, NaN if it abstains; C1r and C2r come per hue source."""
    v = r.get(fn + '_' + st if fn in ('C1r', 'C2r') else fn)
    return np.nan if v is None else float(v)


def wauc(f, y, w):
    """Weighted AUC of feature f for binary label y, pairs weighted by w_i w_j; ties 0.5."""
    o = np.argsort(f, kind='stable')
    f, y, w = f[o], y[o], w[o]
    wp, wn = (w * y).sum(), (w * (1 - y)).sum()
    if wp <= 0 or wn <= 0:
        return np.nan
    # sum over positives of weight x (negative weight below + half the tied negative weight)
    uniq, inv = np.unique(f, return_inverse=True)
    negw = np.bincount(inv, weights=w * (1 - y))
    below = np.concatenate([[0], np.cumsum(negw)[:-1]])
    num = (w * y * (below[inv] + 0.5 * negw[inv])).sum()
    return float(num / (wp * wn))


def best_acc(f, y, w):
    """Weighted accuracy of the best threshold and side: predict 1 above (or below) t."""
    o = np.argsort(f)
    f, y, w = f[o], y[o], w[o]
    tot = w.sum()
    # predicting 1 for all above index i: correct = negatives below + positives above
    neg_below = np.concatenate([[0], np.cumsum(w * (1 - y))])
    pos_above = (w * y).sum() - np.concatenate([[0], np.cumsum(w * y)])
    acc = (neg_below + pos_above) / tot
    return float(max(acc.max(), (1 - acc).max())), float(max((w * y).sum(), (w * (1 - y)).sum()) / tot)


def power(imgs, st):
    rows = [(im['source'], r) for im in imgs for r in im['regions']]
    out = {}
    for lab_name, alt in (('white>hue', 'white'), ('edge>hue', 'edge')):
        for src in ('all', 'si', 'fc', 'syn', 'iain', 'si+syn', 'fc+iain'):
            sel = [(s, r) for s, r in rows if src == 'all' or s == src or (src in HALVES and s in HALVES[src])]
            if not sel:
                continue
            eh = np.array([r['dE_%s_hue' % st] for _, r in sel])
            ea = np.array([r['dE_%s_%s' % (st, alt)] for _, r in sel])
            y = (ea < eh).astype(float)
            size = np.array([r['nscored'] for _, r in sel], float)
            stake = size * np.abs(ea - eh)
            res = {'n': len(sel), 'share_better_size': float((size * y).sum() / size.sum()),
                   'share_better_stake': float((stake * y).sum() / max(stake.sum(), 1e-9))}
            for fn in FEATS:
                f = np.array([feat(r, fn, st) for _, r in sel], float)
                k = np.isfinite(f)
                if k.sum() < 10:
                    continue
                a1 = wauc(f[k], y[k], size[k])
                a2 = wauc(f[k], y[k], stake[k])
                acc, base = best_acc(f[k], y[k], stake[k])
                res[fn] = dict(cover=float(size[k].sum() / size.sum()), auc_size=a1, auc_stake=a2,
                               acc_stake=acc, base_stake=base)
            out['%s|%s' % (lab_name, src)] = res
    return out


def composite(im, choice):
    """dE per mask of the composite: choice maps region id -> fill index, the rest
    takes the fallback, given as choice[None]."""
    fb = choice[None]
    res = {}
    for m in MASKS:
        S = im['S_' + m]
        idx = np.full(S.shape[0], fb)
        for r, c in choice.items():
            if r is not None:
                idx[r] = c
        idx[0] = 0  # outside the zones all fills agree
        tot = S[np.arange(S.shape[0]), idx].sum()
        res[m] = tot / im['n_' + m] if im['n_' + m] >= 50 else np.nan
    return res


def rule_choice(im, rule):
    fn, t, side, hi, lo, fb = rule
    ch = {None: fb}
    for r in im['regions']:
        v = feat(r, fn, im['set'])
        if not np.isfinite(v):
            continue
        ch[r['region']] = hi if (v > t) == (side > 0) else lo
    return ch


def rescues(imgs, choices):
    """Per mask: fixed, worse (on failing), broke (on fine), n failing, n fine, mean."""
    out = {}
    for m in MASKS:
        fx = wo = bk = nf = nk = 0
        vals = []
        for im, ch in zip(imgs, choices):
            if m not in im['others']:
                continue
            g = composite(im, ch)[m]
            if not np.isfinite(g):
                continue
            o = im['others'][m]
            vals.append(g)
            if o > THR:
                nf += 1
                fx += fixed(o, g)
                wo += broke(o, g)
            else:
                nk += 1
                bk += broke(o, g)
        out[m] = dict(fixed=fx, worse=wo, broke=bk, failing=nf, fine=nk, mean=float(np.mean(vals)) if vals else np.nan)
    return out


def objective(r):
    return (r['full']['fixed'] - r['full']['broke'], -r['full']['mean'])


def tune(imgs, fn):
    vals = np.array([feat(r, fn, im['set']) for im in imgs for r in im['regions']])
    vals = vals[np.isfinite(vals)]
    if len(vals) < 10:
        return None
    ts = np.unique(np.quantile(vals, np.linspace(0.05, 0.95, 19)))
    best = None
    for t in ts:
        for side in (1, -1):
            for hi, lo in itertools.permutations(range(3), 2):
                for fb in range(3):
                    rule = (fn, float(t), side, hi, lo, fb)
                    r = rescues(imgs, [rule_choice(im, rule) for im in imgs])
                    if best is None or objective(r) > objective(best[1]):
                        best = (rule, r)
    return best


def coverage(imgs, rule):
    """Share of the fully clipped mask's zone pixels whose fill the rule's feature decides,
    rather than the fallback."""
    dec = tot = 0.0
    for im in imgs:
        S = im['S_full']
        n = im['n_full_z']
        ch = rule_choice(im, rule)
        for r in range(1, len(n)):
            tot += n[r]
            if r in ch:
                dec += n[r]
    return dec / tot if tot else np.nan


def fixed_fill(imgs, c):
    return rescues(imgs, [{None: c} for _ in imgs])


def oracle(imgs, st, mask='full'):
    ch = []
    for im in imgs:
        # per zone the fill with the lowest summed dE over the zone's pixels in the mask;
        # the zones not in the table too, from the sums
        S = im['S_' + mask]
        c = {None: 0}
        for r in range(1, S.shape[0]):
            c[r] = int(np.argmin(S[r]))
        ch.append(c)
    return rescues(imgs, ch)


def fmt(r):
    return '  '.join('%s %d/%d/%d of %d/%d, mean %.2f' % (m, r[m]['fixed'], r[m]['worse'], r[m]['broke'],
                                                          r[m]['failing'], r[m]['fine'], r[m]['mean']) for m in MASKS)


def main():
    a = sys.argv[1:]
    run = a[0]
    st = a[1] if len(a) > 1 and not a[1].startswith('-') else 'model'
    outp = a[a.index('-o') + 1] if '-o' in a else None
    imgs = load(run, st)
    print('set %s: %d images, %d regions' % (st, len(imgs), sum(len(im['regions']) for im in imgs)))
    res = dict(set=st, power=power(imgs, st))
    print('\n== predictive power (AUC by size / by stake, best accuracy by stake vs majority, coverage)')
    for key, v in res['power'].items():
        print('%-20s n=%4d better: %.2f (size) %.2f (stake)' % (key, v['n'], v['share_better_size'], v['share_better_stake']))
        for fn in FEATS:
            if fn in v:
                x = v[fn]
                print('    %-4s auc %.2f / %.2f  acc %.2f vs %.2f  cover %.2f' % (fn, x['auc_size'], x['auc_stake'],
                                                                             x['acc_stake'], x['base_stake'], x['cover']))
    print('\n== rescues: fixed/worse among failing, broke among fine; per mask')
    print('fixed fills and oracle, all images:')
    for c, f in enumerate(FILLS):
        print('  %-6s %s' % (f, fmt(fixed_fill(imgs, c))))
    print('  oracle (per zone best on full) %s' % fmt(oracle(imgs, st)))
    print('  oracle (per zone best on all)  %s' % fmt(oracle(imgs, st, 'all')))
    res['split'] = {}
    for tune_h, test_h in (('si+syn', 'fc+iain'), ('fc+iain', 'si+syn')):
        A = [im for im in imgs if im['source'] in HALVES[tune_h]]
        B = [im for im in imgs if im['source'] in HALVES[test_h]]
        print('\n-- tune on %s (%d), test on %s (%d)' % (tune_h, len(A), test_h, len(B)))
        fx = {f: fixed_fill(B, c) for c, f in enumerate(FILLS)}
        for f in FILLS:
            print('  fixed %-6s test: %s' % (f, fmt(fx[f])))
        print('  oracle       test: %s' % fmt(oracle(B, st)))
        sres = {'fixed': fx, 'rules': {}}
        for fn in FEATS:
            b = tune(A, fn)
            if b is None:
                continue
            rule, rA = b
            rB = rescues(B, [rule_choice(im, rule) for im in B])
            sres['rules'][fn] = dict(rule=rule, tune=rA, test=rB)
            print('  %-4s t=%.3g side %+d above %s below %s fallback %s' % (fn, rule[1], rule[2], FILLS[rule[3]],
                                                                           FILLS[rule[4]], FILLS[rule[5]]))
            print('       tune: %s' % fmt(rA))
            print('       test: %s' % fmt(rB))
            print('       decided by the feature: %.2f of zone pixels in the fully clipped mask (tune), %.2f (test)'
                  % (coverage(A, rule), coverage(B, rule)))
            sres['rules'][fn]['coverage'] = [coverage(A, rule), coverage(B, rule)]
        res['split']['%s->%s' % (tune_h, test_h)] = sres
    if outp:
        json.dump(res, open(outp, 'w'), indent=1, default=lambda o: o.item() if hasattr(o, 'item') else str(o))


if __name__ == '__main__':
    main()

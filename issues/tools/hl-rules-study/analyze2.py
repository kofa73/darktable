# analyze2.py <run dir> [set] [-o out.json]: two-feature selectors for Phase 2, greedy.
# Stage 1 is the single-feature rule of analyze.py, tuned on one half. Stage 2 overrides
# the fill of the regions that stage 1 sent to its "below" branch: regions whose second
# feature lies on the chosen side of the threshold take another fill. Stage 2 is tuned on
# the same half with the same objective (fully clipped mask: fixed minus broken among the
# fine images, ties by mean dE), for the three best stage-1 features of that half, then
# scored on the other half. Same splits as analyze.py.
import sys, os, json
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze as A


def choice2(im, rule1, rule2):
    ch = A.rule_choice(im, rule1)
    fn, t, side, fill = rule2
    for r in im['regions']:
        if ch.get(r['region']) != rule1[4]:
            continue
        v = A.feat(r, fn, im['set'])
        if np.isfinite(v) and (v > t) == (side > 0):
            ch[r['region']] = fill
    return ch


def tune2(imgs, rule1):
    best = None
    for fn in A.FEATS:
        vals = np.array([A.feat(r, fn, im['set']) for im in imgs for r in im['regions']])
        vals = vals[np.isfinite(vals)]
        if len(vals) < 10:
            continue
        for t in np.unique(np.quantile(vals, np.linspace(0.05, 0.95, 19))):
            for side in (1, -1):
                for fill in range(3):
                    if fill == rule1[4]:
                        continue
                    rule2 = (fn, float(t), side, fill)
                    r = A.rescues(imgs, [choice2(im, rule1, rule2) for im in imgs])
                    if best is None or A.objective(r) > A.objective(best[1]):
                        best = (rule2, r)
    return best


def main():
    a = sys.argv[1:]
    run = a[0]
    st = a[1] if len(a) > 1 and not a[1].startswith('-') else 'model'
    outp = a[a.index('-o') + 1] if '-o' in a else None
    imgs = A.load(run, st)
    res = dict(set=st, split={})
    for tune_h, test_h in (('si+syn', 'fc+iain'), ('fc+iain', 'si+syn')):
        TA = [im for im in imgs if im['source'] in A.HALVES[tune_h]]
        TB = [im for im in imgs if im['source'] in A.HALVES[test_h]]
        print('\n-- set %s, tune on %s (%d), test on %s (%d)' % (st, tune_h, len(TA), test_h, len(TB)), flush=True)
        stage1 = [b for b in (A.tune(TA, fn) for fn in A.FEATS) if b is not None]
        stage1.sort(key=lambda b: A.objective(b[1]), reverse=True)
        out = []
        for rule1, r1 in stage1[:3]:
            b = tune2(TA, rule1)
            rule2, rA = b
            rB = A.rescues(TB, [choice2(im, rule1, rule2) for im in TB])
            print('  %s t=%.3g side %+d above %s below %s fallback %s; then %s t=%.3g side %+d -> %s'
                  % (rule1[0], rule1[1], rule1[2], A.FILLS[rule1[3]], A.FILLS[rule1[4]], A.FILLS[rule1[5]],
                     rule2[0], rule2[1], rule2[2], A.FILLS[rule2[3]]))
            print('       tune: %s' % A.fmt(rA))
            print('       test: %s' % A.fmt(rB), flush=True)
            out.append(dict(rule1=rule1, rule2=rule2, tune=rA, test=rB))
        res['split']['%s->%s' % (tune_h, test_h)] = out
    if outp:
        json.dump(res, open(outp, 'w'), indent=1, default=lambda o: o.item() if hasattr(o, 'item') else str(o))


if __name__ == '__main__':
    main()

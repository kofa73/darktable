# phase5.py <tune run> <test run> [-o out.json]: the near-miss rules of Phase 2 tuned on all
# images of the tune run (the 173 of runs/base) and scored on the test run (TZ101
# synthetic, runs/tz), which played no part in any tuning. Single-feature rules C2, C2r,
# D1, D2 for both hue sources, and the greedy two-feature selector of analyze2.py (best
# three stage-1 rules), against the fixed fills and the oracle of the test run.
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze as A
import analyze2 as A2


def main():
    a = sys.argv[1:]
    outp = a[a.index('-o') + 1] if '-o' in a else None
    res = {}
    for st in ('model', 'cont'):
        T = A.load(a[0], st)
        X = A.load(a[1], st)
        print('\n-- set %s: tune on %d images, test on %d' % (st, len(T), len(X)), flush=True)
        r = dict(fixed={f: A.fixed_fill(X, c) for c, f in enumerate(A.FILLS)}, oracle=A.oracle(X, st), rules={})
        for f in A.FILLS:
            print('  fixed %-6s %s' % (f, A.fmt(r['fixed'][f])))
        print('  oracle       %s' % A.fmt(r['oracle']))
        stage1 = []
        for fn in A.FEATS:
            b = A.tune(T, fn)
            if b is None:
                continue
            stage1.append(b)
            if fn in ('C2', 'C2r', 'D1', 'D2'):
                rule = b[0]
                rX = A.rescues(X, [A.rule_choice(im, rule) for im in X])
                r['rules'][fn] = dict(rule=rule, tune=b[1], test=rX)
                print('  %-4s t=%.3g side %+d above %s below %s fallback %s' % (fn, rule[1], rule[2], A.FILLS[rule[3]],
                                                                               A.FILLS[rule[4]], A.FILLS[rule[5]]))
                print('       tune: %s' % A.fmt(b[1]))
                print('       test: %s' % A.fmt(rX), flush=True)
        stage1.sort(key=lambda b: A.objective(b[1]), reverse=True)
        r['two'] = []
        for rule1, _ in stage1[:3]:
            rule2, rT = A2.tune2(T, rule1)
            rX = A.rescues(X, [A2.choice2(im, rule1, rule2) for im in X])
            print('  %s then %s t=%.3g side %+d -> %s' % (rule1[0], rule2[0], rule2[1], rule2[2], A.FILLS[rule2[3]]))
            print('       tune: %s' % A.fmt(rT))
            print('       test: %s' % A.fmt(rX), flush=True)
            r['two'].append(dict(rule1=rule1, rule2=rule2, tune=rT, test=rX))
        res[st] = r
    if outp:
        json.dump(res, open(outp, 'w'), indent=1, default=lambda o: o.item() if hasattr(o, 'item') else str(o))


if __name__ == '__main__':
    main()

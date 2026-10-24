# auc_table.py: markdown tables of the Phase 2 predictive power (runs/phase2-<set>.json),
# weighted AUC by stake (by size in parentheses) per feature and source
import json
for st in ('model', 'cont'):
    P = json.load(open('/tmp/hl-rules-study/runs/phase2-%s.json' % st))['power']
    for lab in ('white>hue', 'edge>hue'):
        keys = [k for k in P if k.startswith(lab + '|')]
        print('\n%s %s, AUC by stake (by size)\n' % (st, lab))
        print('| feature | ' + ' | '.join(k.split('|')[1] for k in keys) + ' |')
        print('|---' * (len(keys) + 1) + '|')
        for fn in ['A1', 'A2', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6', 'B7', 'C1', 'C2', 'C1r', 'C2r', 'D1', 'D2']:
            print('| %s | ' % fn + ' | '.join('%.2f (%.2f)' % (P[k][fn]['auc_stake'], P[k][fn]['auc_size'])
                                            if fn in P[k] else '-' for k in keys) + ' |')
        print('| regions | ' + ' | '.join(str(P[k]['n']) for k in keys) + ' |')
        print('| share better, by stake | ' + ' | '.join('%.2f' % P[k]['share_better_stake'] for k in keys) + ' |')

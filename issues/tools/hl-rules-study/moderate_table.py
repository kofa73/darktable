# moderate_table.py <run dir> [mask] [out.txt]: per case (scene, k), mean dE on the mask (default
# all) for inpaint opposed and its variants, segmentation based at defaults, the best of the
# slider sweep without and with the threshold fix (setting in brackets; best by the truth),
# the scale and rebuild variants, and gradient domain "continuous border, hue estimate in fully
# clipped"; then means over the cases.
import json, glob, os, sys
import numpy as np
run = sys.argv[1]; mk = sys.argv[2] if len(sys.argv) > 2 else 'all'
COLS = ['opposed', 'opp-cons', 'opp-asshot', 'segments', 'sweep', 'sweep-t', 'seg-full', 'seg-asshot',
        'segrb', 'segrb-thresh', 'segrb-thresh-cbrt', 'gd-wb4x10']
rows = []
for f in sorted(glob.glob(os.path.join(run, '*', 'scores.json'))):
    s = json.load(open(f)); m = s['methods']
    g = lambda k: m[k][mk]['dE_mean'] if k in m and mk in m[k] else np.nan
    sw = {k: g(k) for k in m if k.startswith('seg-c') and not k.endswith('-t')}
    swt = {k: g(k) for k in m if k.startswith('seg-c') and k.endswith('-t')}
    b = min(sw, key=sw.get) if sw else None; bt = min(swt, key=swt.get) if swt else None
    r = {c: g(c) for c in COLS if c not in ('sweep', 'sweep-t')}
    r['sweep'] = sw[b] if b else np.nan; r['sweep-t'] = swt[bt] if bt else np.nan
    rows.append((os.path.basename(os.path.dirname(f))[4:], s.get('clipped_photosites', np.nan), r, b, bt))
out = ['mask %s, mean dE; sweep = best of combine {1,2,4} x candidating {0.2,0.5,0.8}, -t with threshold fix' % mk,
       '%-30s %6s ' % ('case', 'clip%') + ' '.join('%8s' % c[:8] for c in COLS) + '  best sweep / with fix']
for n, cp, r, b, bt in rows:
    out.append('%-30s %6.2f ' % (n[:30], 100 * cp) + ' '.join('%8.2f' % r[c] for c in COLS) + '  %s / %s' % (b, bt))
A = np.array([[r[c] for c in COLS] for _, _, r, _, _ in rows])
out.append('%-30s %6s ' % ('mean', '') + ' '.join('%8.2f' % v for v in np.nanmean(A, 0)))
txt = '\n'.join(out); print(txt)
if len(sys.argv) > 3:
    open(sys.argv[3], 'w').write(txt + '\n')

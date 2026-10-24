# structure.py <run> <plan.json> [mask]: does a method recover the lightness structure of
# the clipped areas? Per image, inside the mask, on the display PNGs (_d) of an
# EV2_DISPLAY run: the correlation between the method's and the truth's L* gradients
# (1 = same detail, 0 = none), and the ratio of their local contrast (std of high-passed
# L*; below 1 = flatter than the truth, the "featureless" look).
import sys, json, os, numpy as np, cv2
run, plan = sys.argv[1], json.load(open(sys.argv[2])); mk = sys.argv[3] if len(sys.argv) > 3 else 'all'
SUF = os.environ.get('SUF', '_d')
M = ['opposed', 'segments', 'gd-wb4x7', 'gd-wb4x8']
def L(p):
    return cv2.cvtColor(cv2.imread(p), cv2.COLOR_BGR2Lab)[..., 0].astype(np.float32) * (100 / 255)
print('%-26s %6s  %s' % ('image', 'pixels', '   '.join('%-14s' % m for m in M)))
print('%-26s %6s  %s' % ('', '', '   '.join('%-14s' % 'gcorr  contrast' for m in M)))
agg = {m: [] for m in M}
for e in plan:
    wd = os.path.join(run, e['name'])
    if not os.path.exists(wd + '/scores.json'):
        continue
    msk = np.load(wd + '/masks.npz')[mk]
    if msk.sum() < 500:
        continue
    t = L(wd + '/truth%s.png' % SUF)
    g = lambda a: (cv2.Sobel(a, cv2.CV_32F, 1, 0), cv2.Sobel(a, cv2.CV_32F, 0, 1))
    hp = lambda a: a - cv2.GaussianBlur(a, (0, 0), 3)
    tx, ty = g(t); th = hp(t)[msk].std()
    row = []
    for m in M:
        a = L('%s/%s%s.png' % (wd, m, SUF)); ax, ay = g(a)
        v1 = np.concatenate([tx[msk], ty[msk]]); v2 = np.concatenate([ax[msk], ay[msk]])
        gc = float(np.corrcoef(v1, v2)[0, 1]) if v1.std() > 1e-6 and v2.std() > 1e-6 else 0.0;  # flat: no structure
        cr = float(hp(a)[msk].std() / max(th, 1e-6))
        agg[m].append((gc, cr)); row.append('%5.2f  %5.2f    ' % (gc, cr))
    print('%-26s %6d  %s' % (e['name'][:26], msk.sum(), ' '.join(row)))
print('%-26s %6s  %s' % ('median', '', ' '.join('%5.2f  %5.2f    ' % tuple(np.median(np.array(agg[m]), 0)) for m in M)))

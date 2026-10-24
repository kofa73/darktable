import sys
import numpy as np
from PIL import Image
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
crop = lambda a: np.rot90(a, 1)[:3008]
inp, st, pre = crop(d['inp']), crop(d['st']), crop(d['pre'])
f = st > 0
nb = np.zeros(f.shape, np.int8)
nb[1:, :] += f[:-1, :]; nb[:-1, :] += f[1:, :]; nb[:, 1:] += f[:, :-1]; nb[:, :-1] += f[:, 1:]
# measured sky colors next to the clipped region; bright ones only, to leave out pixels mixed with dark objects
border = (~f) & (nb > 0) & (inp[..., 1] > 0.8)
gr, br = inp[..., 1] / inp[..., 0], inp[..., 2] / inp[..., 0]
x, y = br[border], gr[border]
A = np.stack([np.ones_like(x), x, x * x], 1)
coef, *_ = np.linalg.lstsq(A, y, rcond=None)
res = y - A @ coef
print('border pixels %d; G/R = %.3f + %.3f B/R + %.3f (B/R)^2; residual std %.3f; G/R range %.3f-%.3f; B/R range %.3f-%.3f' %
      ((border.sum(),) + tuple(coef) + (res.std(),) + tuple(np.percentile(y, [2, 98])) + tuple(np.percentile(x, [2, 98]))))
# inside the green-only region: green predicted from the measured B/R, against the restored green
m2 = st == 2
out_g = np.maximum(pre[..., 1], inp[..., 1])
pred = (coef[0] + coef[1] * br + coef[2] * br * br) * inp[..., 0]
q = (out_g / pred)[m2]
print('class 2: restored G / G predicted from B/R: p5 %.3f p25 %.3f p50 %.3f p75 %.3f p95 %.3f' % tuple(np.percentile(q, [5, 25, 50, 75, 95])))
# the same for the region right of the tower and the halo next to it (export coordinates x2.006)
for name, (ys, xs) in {'halo right of tower': ((2306, 2507), (2270, 2330)),
                       'sky right of tower': ((2306, 2507), (2450, 2650)),
                       'upper sky, middle': ((600, 800), (1600, 2000))}.items():
    sl = (slice(*ys), slice(*xs))
    mm = m2[sl]
    if mm.sum():
        print('%-20s n %6d: restored/predicted G %.3f, measured B/R %.3f' % (name, mm.sum(), (out_g / pred)[sl][mm].mean(), br[sl][mm].mean()))
# map: green = restored green above the prediction, magenta = below, dark gray = not class 2
t = np.clip(np.nan_to_num(out_g / pred - 1, nan=0) / 0.06, -1, 1)
img = np.stack([0.5 - 0.5 * t, 0.5 + 0.5 * t, 0.5 - 0.5 * t], -1)
img[~m2] = 0.15
Image.fromarray((img[::4, ::4] * 255).astype(np.uint8)).resize((800, 600)).save('green-vs-pred.png')

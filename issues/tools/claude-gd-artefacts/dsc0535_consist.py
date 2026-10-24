import sys
import numpy as np
from PIL import Image
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
crop = lambda a: np.rot90(a, 1)[:3008]
inp, st, rho, pre = crop(d['inp']), crop(d['st']), crop(d['rho']), crop(d['pre'])
# where only green is clipped, red and blue are measured: compare the B/R of the hue estimate with the measured B/R
m2 = st == 2
err = np.full(st.shape, np.nan, np.float32)
err[m2] = (rho[..., 2] / np.maximum(rho[..., 0], 1e-4) / (inp[..., 2] / inp[..., 0]))[m2]
v = err[m2]
print('class 2: rho B/R over measured B/R: p5 %.3f p25 %.3f p50 %.3f p75 %.3f p95 %.3f' % tuple(np.percentile(v, [5, 25, 50, 75, 95])))
# map: blue = estimate too blue, red = estimate not blue enough, gray = not class 2
e = np.nan_to_num(err, nan=1.0)
t = np.clip((e - 1) / 0.15, -1, 1)
img = np.stack([0.5 - 0.5 * np.minimum(t, 0), 0.5 - 0.5 * np.abs(t), 0.5 + 0.5 * np.maximum(t, 0)], -1)
img[~m2] = 0.15
Image.fromarray((img[::4, ::4] * 255).astype(np.uint8)).save('consist.png')
# the hue estimate's G/R and B/R as images, to see where each part of it comes from
for k, name in ((1, 'gr'), (2, 'br')):
    q = rho[..., k] / np.maximum(rho[..., 0], 1e-4)
    q[st == 0] = np.nan
    lo, hi = np.nanpercentile(q, [2, 98])
    g = np.clip((np.nan_to_num(q, nan=lo) - lo) / (hi - lo), 0, 1)
    g[st == 0] = 0
    print('rho %s: 2%% %.3f 98%% %.3f' % (name, lo, hi))
    Image.fromarray((g[::4, ::4] * 255).astype(np.uint8)).save('rho-%s.png' % name)

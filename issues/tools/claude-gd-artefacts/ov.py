# ov.py <ev2 workdir> <out.jpg>: truth with the partial (green) and full (red) masks, and excluded pixels (blue)
import sys, numpy as np
from PIL import Image
wd, out = sys.argv[1], sys.argv[2]
a = np.asarray(Image.open(wd + '/truth.png')).astype(np.float32) / 255
m = np.load(wd + '/masks.npz')
ov = a.copy()
for k, c in (('partial', (0, 1, 0)), ('full', (1, 0, 0))):
    ov[m[k]] = 0.5 * ov[m[k]] + 0.5 * np.array(c)
ex = (m['F'].astype(np.float32).max(2) > 0.5) & ~m['all']
ov[ex] = 0.5 * ov[ex] + 0.5 * np.array([0, 0, 1])
Image.fromarray((np.concatenate([a, ov], 1) * 255).astype(np.uint8)).save(out, quality=90)

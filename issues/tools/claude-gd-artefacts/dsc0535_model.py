import sys
import numpy as np
from scipy import ndimage as ndi
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
inp, st, rho, pre = d['inp'], d['st'], d['rho'], d['pre']
gflag = (st & 2) > 0
# distance of each green-flagged pixel to the nearest pixel whose green is a boundary value
dist = ndi.distance_transform_edt(gflag)
model_r = rho[..., 1] / np.maximum(rho[..., 0], 1e-4) * inp[..., 0]
model_b = rho[..., 1] / np.maximum(rho[..., 2], 1e-4) * inp[..., 2]
for cls in (2, 6):
    for lo, hi in ((1, 3), (3, 10), (10, 30), (30, 100), (100, 1e9)):
        m = (st == cls) & (dist >= lo) & (dist < hi)
        if m.sum() < 100: continue
        print('class %d dist %3d-%-4s n %8d  G in %.3f solved %.3f  model(R) %.3f model(B) %.3f  R/rhoR %.3f' %
              (cls, lo, '' if hi > 1e8 else int(hi), m.sum(), inp[..., 1][m].mean(), pre[..., 1][m].mean(),
               model_r[m].mean(), model_b[m].mean(), (inp[..., 0] / rho[..., 0])[m].mean()))

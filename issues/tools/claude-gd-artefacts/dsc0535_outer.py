import sys
import numpy as np
from scipy import ndimage as ndi
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
inp, st, rho = d['inp'], d['st'], d['rho']
f = st > 0
# close the speckle: an unflagged pixel inside the region is not a real boundary
closed = ndi.binary_closing(f, structure=np.ones((5, 5), bool), border_value=0) | f
outer = (~closed) & ndi.binary_dilation(closed, structure=ndi.generate_binary_structure(2, 1))
print('flagged %d, closed %d, outer border %d' % (f.sum(), closed.sum(), outer.sum()))
# nearest outer border pixel for every pixel
dist, (iy, ix) = ndi.distance_transform_edt(~outer, return_indices=True)
for cls, c in ((2, 0), (2, 2), (6, 0)):
    m = st == cls
    r = inp[..., c][m] / inp[..., c][iy[m], ix[m]]
    dd = dist[m]
    print('class %d chan %d vs nearest outer border: p10 %.3f p50 %.3f p90 %.3f p99 %.3f; distance p50 %.0f p90 %.0f px' %
          ((cls, c) + tuple(np.percentile(r, [10, 50, 90, 99])) + tuple(np.percentile(dd, [50, 90]))))
# brightness profile: unclipped red in class 6 against distance from the outer border
m = st == 6
for lo, hi in ((0, 10), (10, 50), (50, 150), (150, 400), (400, 2000)):
    s = m & (dist >= lo) & (dist < hi)
    if s.sum():
        print('class 6, distance %4d-%4d: n %8d  in R %.3f  rho R %.3f  rho G %.3f rho B %.3f' %
              (lo, hi, s.sum(), inp[..., 0][s].mean(), rho[..., 0][s].mean(), rho[..., 1][s].mean(), rho[..., 2][s].mean()))
print('outer border color mean', np.round(inp[outer].mean(0), 3))

import sys
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
inp, st, rho, pre = d['inp'], d['st'], d['rho'], d['pre']
# how far the unclipped channel rises above the hue estimate (which is the border color, interpolated)
for cls, c in ((2, 0), (2, 2), (6, 0)):
    m = st == cls
    r = inp[..., c][m] / rho[..., c][m]
    print('class %d chan %d: in/rho p10 %.3f p50 %.3f p90 %.3f p99 %.3f' % ((cls, c) + tuple(np.percentile(r, [10, 50, 90, 99]))))
# unflagged pixels with 4 flagged neighbors: Dirichlet values inside the clipped region
f = st > 0
nb = np.zeros_like(st, np.int32)
nb[1:, :] += f[:-1, :]; nb[:-1, :] += f[1:, :]; nb[:, 1:] += f[:, :-1]; nb[:, :-1] += f[:, 1:]
border = (~f) & (nb > 0)
holes = (~f) & (nb == 4)
print('border pixels %d, of them with all 4 neighbors flagged %d' % (border.sum(), holes.sum()))
print('green of border pixels: p10 %.4f p50 %.4f p90 %.4f' % tuple(np.percentile(inp[..., 1][border], [10, 50, 90])))
# green per 2x2 phase in the interior of the green-clipped region: photosite values vs interpolated ones
interior = f.copy()
for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, 2), (0, -2)):
    interior &= np.roll(np.roll(st & 2, dy, 0), dx, 1) > 0
g = inp[..., 1]
for py in range(2):
    for px in range(2):
        v = g[py::2, px::2][interior[py::2, px::2]] * 3880
        print('phase %d%d: n %d green raw p1 %.0f p10 %.0f p50 %.0f p90 %.0f p99 %.0f' % ((py, px, v.size) + tuple(np.percentile(v, [1, 10, 50, 90, 99]))))

import sys
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
inp = np.rot90(d['inp'], 1); st = np.rot90(d['st'], 1); rho = np.rot90(d['rho'], 1)
y0, y1 = 2306, 2507
f = st > 0
nb = np.zeros(f.shape, np.int8)
nb[1:, :] += f[:-1, :]; nb[:-1, :] += f[1:, :]; nb[:, 1:] += f[:, :-1]; nb[:, :-1] += f[:, 1:]
border = (~f) & (nb > 0)
print('%5s %7s %7s | %-21s | %s' % ('X', 'flagged', 'border', 'input R G B', 'G/R B/R of the input'))
for X in range(2238, 2262):
    sl = (slice(y0, y1), X)
    i = inp[sl].mean(0)
    print('%5d %7.2f %7.2f | %6.3f %6.3f %6.3f | %.3f %.3f' % (X, f[sl].mean(), border[sl].mean(), i[0], i[1], i[2], i[1] / i[0], i[2] / i[0]))
# hue of the border pixels by brightness: dim ones are mixed with the tower
b = border[y0:y1, 2200:2300]
v = inp[y0:y1, 2200:2300][b]
for lo, hi in ((0, 0.3), (0.3, 0.6), (0.6, 0.9), (0.9, 1.0)):
    m = (v[:, 1] >= lo) & (v[:, 1] < hi)
    if m.sum():
        print('border pixels at the tower edge with G in [%.1f, %.1f): n %4d, G/R %.3f, B/R %.3f' % (lo, hi, m.sum(), (v[m, 1] / v[m, 0]).mean(), (v[m, 2] / v[m, 0]).mean()))
# the sky far from any border: unclipped channels only

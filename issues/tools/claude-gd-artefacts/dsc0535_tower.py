import sys
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import hlcdump
d = hlcdump.load(sys.argv[1])
# to export orientation: the photo is rotated 90 degrees counter-clockwise; the export scale is 2000/4012
rot = {k: np.rot90(d[k], 1) for k in ('inp', 'st', 'rho', 'pre')}
s = 4012 / 2000
inp, st, rho, pre = rot['inp'], rot['st'], rot['rho'], rot['pre']
out = inp.copy()
for c in range(3):
    m = (st >> c) & 1 == 1
    out[..., c][m] = np.maximum(pre[..., c][m], inp[..., c][m])
y0, y1 = int(1150 * s), int(1250 * s)
print('rows %d-%d (sensor px), export x -> sensor X' % (y0, y1))
print('%6s %6s %5s | %-20s | %-20s | %-20s | %s' % ('x', 'X', 'flag', 'input R G B', 'rho R G B', 'output R G B', 'G/R rho, out; B/R rho, out'))
for x in list(range(1130, 1160, 3)) + list(range(1160, 1400, 20)):
    X = int(x * s)
    sl = (slice(y0, y1), slice(X, X + 2))
    f = st[sl]
    fl = ''.join(str(int(((f >> c) & 1).mean() * 9)) for c in range(3))
    i, r, o = inp[sl].reshape(-1, 3).mean(0), rho[sl].reshape(-1, 3).mean(0), out[sl].reshape(-1, 3).mean(0)
    print('%6d %6d %5s | %6.3f %6.3f %6.3f | %6.3f %6.3f %6.3f | %6.3f %6.3f %6.3f | %.3f %.3f; %.3f %.3f' %
          ((x, X, fl) + tuple(i) + tuple(r) + tuple(o) + (r[1] / r[0], o[1] / o[0], r[2] / r[0], o[2] / o[0])))
np.save('rot_st.npy', st)

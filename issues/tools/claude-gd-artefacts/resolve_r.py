# Rebuilds hlcolor's Poisson system for one channel (smooth fill, skip rule) from a
# TEMPDEBUG dump and solves it with a direct solver, for comparison with the CG result.
import sys; sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import numpy as np, scipy.sparse as sp, scipy.sparse.linalg as spl
from hlcdump import load
path, j = sys.argv[1], int(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else 'code'  # code: r_mean * dB as in hlcolor; integrable: d(r B); const: one ratio per channel pair
d = load(path)
st, f, inp, pre, rho = d['st'].ravel(), d['filled'].ravel(), d['inp'].reshape(-1, 3).astype(np.float64), d['pre'].reshape(-1, 3).astype(np.float64), d['rho'].reshape(-1, 3).astype(np.float64)
w, h = d['w'], d['h']
clipmode = sys.argv[4] if len(sys.argv) > 4 else 'min'
# hlcolor flags by threshold, so the smallest flagged value is the clip level; on a
# Bayer grid a flagged window mean can be lower, but clipped photosites reach the maximum
clip = np.array([inp[:, c][((st >> c) & 1).astype(bool)].min() if clipmode == 'min' else inp[:, c].max() for c in range(3)])
inv_clip = 1 / clip
# working copy before the solves: input, with the smooth fill in the filled channels
cur = inp.copy()
for c in range(3):
    m = ((f >> c) & 1).astype(bool)
    cur[m, c] = pre[m, c]
usable = (~st.astype(np.int64) | f) & 7
def weight(v):
    n = np.clip(v, 0, 1)
    x = np.where(n <= 0.65, n / 0.65, (1 - n) / 0.35)
    return x * x * (3 - 2 * x) + 1e-3
def ratio(i, k):
    return np.maximum(rho[i, j], 0) / np.maximum(rho[i, k], 1e-4)
unk = np.nonzero(((st >> j) & 1).astype(bool) & ~((f >> j) & 1).astype(bool))[0]
n = len(unk)
rc = [np.median(ratio(unk, k)) for k in range(3)]
mp = np.full(w * h, -1); mp[unk] = np.arange(n)
row, col = unk // w, unk % w
rhs = np.zeros(n); diag = np.zeros(n)
I, J = [], []
for dy, dx in ((0, -1), (0, 1), (-1, 0), (1, 0)):
    rr, cc = row + dy, col + dx
    ok = (rr >= 0) & (rr < h) & (cc >= 0) & (cc < w)
    p = unk[ok]; q = rr[ok] * w + cc[ok]; ki = np.nonzero(ok)[0]
    diag[ki] += 1
    us = usable[p] & usable[q]
    num = np.zeros(len(p)); den = np.zeros(len(p))
    for k in range(3):
        if k == j: continue
        use = ((us >> k) & 1).astype(bool)
        wt = np.minimum(weight(cur[p, k] * inv_clip[k]), weight(cur[q, k] * inv_clip[k]))
        if mode == 'integrable':
            t = ratio(q, k) * cur[q, k] - ratio(p, k) * cur[p, k]
        elif mode == 'const':
            t = rc[k] * (cur[q, k] - cur[p, k])
        else:
            t = 0.5 * (ratio(p, k) + ratio(q, k)) * (cur[q, k] - cur[p, k])
        num += np.where(use, wt * t, 0); den += np.where(use, wt, 0)
    g = np.where(den > 0, num / np.where(den > 0, den, 1), 0)
    rhs[ki] -= g
    isunk = mp[q] >= 0
    np.add.at(rhs, ki[~isunk], inp[q[~isunk], j])
    I.append(ki[isunk]); J.append(mp[q[isunk]])
I = np.concatenate(I); J = np.concatenate(J)
A = sp.csr_matrix((np.concatenate([diag, -np.ones(len(I))]), (np.concatenate([np.arange(n), I]), np.concatenate([np.arange(n), J]))), shape=(n, n))
if n < 200000:
    x = spl.spsolve(A.tocsc(), rhs)
else:
    import pyamg
    x = pyamg.ruge_stuben_solver(A.tocsr()).solve(rhs, tol=1e-12, maxiter=200)
cg = pre[unk, j]
print('channel', j, 'unknowns', n, 'clip', clip[j])
print('direct: min %.4g max %.4g (x clip)' % (x.min() / clip[j], x.max() / clip[j]))
print('CG    : min %.4g max %.4g (x clip)' % (cg.min() / clip[j], cg.max() / clip[j]))
print('direct < 0: %d pixels, CG < 0: %d pixels' % ((x < 0).sum(), (cg < 0).sum()))
print('max |CG - direct| / clip = %.4g' % (np.abs(cg - x).max() / clip[j]))
print('residual of CG result relative to |rhs|: %.3g' % (np.linalg.norm(A @ cg - rhs) / np.linalg.norm(rhs)))
np.save(path + f'.direct{j}.npy', np.stack([unk, x]))

# Prototype: correct the hue estimate of the gradient domain method with the
# channels that are still measured inside the clipped region, then redo the
# channel solves as _gd_solve() in src/iop/hlreconstruct/gradient.c does.
#
# usage: hue_proto.py <GD dump> <outdir> <clip R,G,B> [lambda] [along|line]
#
# The paper's hue estimate (rho) is a Laplace fill of the border colors. Where
# only one channel is clipped, the other two give one color ratio exactly, so
# the error of rho in that ratio is known there. The border colors of a scene
# like a sunset sky lie close to a line in the (log G/R, log B/R) plane; the
# prototype assumes rho is off along that line, so one scalar t per pixel
# (the shift along the line) explains the measured error:
#   rho'_G = rho_G exp(t v1), rho'_B = rho_B exp(t v2), rho'_R = rho_R
# with (v1, v2) the unit direction of the line. t is measured in the pixels
# with one channel clipped, smoothed, and filled into the pixels with two or
# three channels clipped by one sparse solve:
#   minimize  sum over measured pixels (t - t_measured)^2 + lambda sum over edges (t_p - t_q)^2
# with t = 0 on the border, where rho already equals the border colors.
#
# Mode "along" (default) corrects rho only along the line. Mode "line" first
# projects rho onto the line inside U, which removes errors across the line
# that the measured ratio cannot see, such as border pixels mixed with a dark
# object; where only G is clipped, G/R then follows the line from the measured
# B/R. The border pixels keep their rho, so the solution still starts from the
# measured values at the border.
#
# Writes to outdir:
#   orig.grid, corr-<mode>.grid   solved RGBA grids for GD_LOAD (int32 w, h, float32 RGBA),
#                                 with the original and the corrected hue estimate
#   proto-<mode>.npz              t, the corrected rho and both solutions
import sys, os, time
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import numpy as np, scipy.sparse as sp, pyamg
import hlcdump

path, outdir = sys.argv[1], sys.argv[2]
clip = np.array([float(v) for v in sys.argv[3].split(',')])
lam = float(sys.argv[4]) if len(sys.argv) > 4 else 25.0
mode = sys.argv[5] if len(sys.argv) > 5 else 'along'
os.makedirs(outdir, exist_ok=True)
t0 = time.time()

d = hlcdump.load(path)
w, h = d['w'], d['h']
npix = w * h
st = d['st'].ravel().astype(np.int64)
inp = d['inp'].reshape(-1, 3).astype(np.float64)
rho = d['rho'].reshape(-1, 3).astype(np.float64)
pre_c = d['pre'].reshape(-1, 3).astype(np.float64)
del d
inv_clip = 1 / clip
U = st > 0

def neighbors(idx):
    # yields (mask of idx with an in-image neighbor in that direction, neighbor index)
    row, col = idx // w, idx % w
    for dy, dx in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        rr, cc = row + dy, col + dx
        ok = (rr >= 0) & (rr < h) & (cc >= 0) & (cc < w)
        yield ok, rr[ok] * w + cc[ok]

def laplacian(unk):
    # graph Laplacian over the unknowns: neighbors outside the image are left out
    # (Neumann), other neighbors that are not unknowns are fixed values (Dirichlet)
    n = len(unk)
    mp = np.full(npix, -1); mp[unk] = np.arange(n)
    diag = np.zeros(n); I = []; J = []
    for ok, q in neighbors(unk):
        ki = np.nonzero(ok)[0]
        diag[ki] += 1
        isunk = mp[q] >= 0
        I.append(ki[isunk]); J.append(mp[q[isunk]])
    I = np.concatenate(I); J = np.concatenate(J)
    A = sp.csr_matrix((np.concatenate([diag, -np.ones(len(I))]),
                       (np.concatenate([np.arange(n), I]), np.concatenate([np.arange(n), J]))), shape=(n, n))
    return A, mp

def amg_solve(A, b, x0=None):
    ml = pyamg.smoothed_aggregation_solver(A, symmetry='symmetric')
    res = []
    x = ml.solve(b, x0=x0, tol=1e-9, accel='cg', maxiter=300, residuals=res)
    return x, len(res) - 1, res[-1] / res[0]

def weight(v):
    # paper eq. 8 as in _gd_weight()
    n = np.clip(v, 0, 1)
    x = np.where(n <= 0.65, n / 0.65, (1 - n) / 0.35)
    return x * x * (3 - 2 * x) + 1e-3

usable = (~st) & 7  # basic experiment: only unflagged channels are references

def solve_channel(j, hue):
    unk = np.nonzero((st >> j) & 1)[0]
    A, mp = laplacian(unk)
    rhs = np.zeros(len(unk))
    rj = np.maximum(hue[:, j], 0)
    for ok, q in neighbors(unk):
        ki = np.nonzero(ok)[0]
        p = unk[ki]
        us = usable[p] & usable[q]
        num = np.zeros(len(p)); den = np.zeros(len(p))
        for k in range(3):
            if k == j: continue
            use = ((us >> k) & 1).astype(bool)
            wt = np.minimum(weight(inp[p, k] * inv_clip[k]), weight(inp[q, k] * inv_clip[k]))
            # the change of r u_k along the edge, r = rho_j / rho_k, as in _gd_edge_guidance()
            g = rj[q] / np.maximum(hue[q, k], 1e-4) * inp[q, k] - rj[p] / np.maximum(hue[p, k], 1e-4) * inp[p, k]
            num += np.where(use, wt * g, 0); den += np.where(use, wt, 0)
        rhs[ki] -= np.where(den > 0, num / np.where(den > 0, den, 1), 0)
        bnd = mp[q] < 0
        np.add.at(rhs, ki[bnd], inp[q[bnd], j])
    x, it, rel = amg_solve(A, rhs, x0=pre_c[unk, j])
    return unk, x, it, rel

# 1. the line of the border colors in the (log G/R, log B/R) plane
f2 = U.reshape(h, w)
nb = np.zeros((h, w), np.int8)
nb[1:, :] += f2[:-1, :]; nb[:-1, :] += f2[1:, :]; nb[:, 1:] += f2[:, :-1]; nb[:, :-1] += f2[:, 1:]
border = ((~f2) & (nb > 0)).ravel()
# dark border pixels are mixed with dark objects, and their ratios are noisy
bsel = border & ((inp * inv_clip).min(1) >= 0.25)
lg = np.log(np.maximum(inp, 1e-6))
a, b = lg[:, 1] - lg[:, 0], lg[:, 2] - lg[:, 0]
P = np.stack([a[bsel], b[bsel]], 1)
cov = np.cov((P - P.mean(0)).T)
ev, evec = np.linalg.eigh(cov)
v = evec[:, 1] * np.sign(evec[1, 1])
print('border pixels used %d of %d; line direction (log G/R, log B/R) = (%.3f, %.3f); '
      'variance along the line %.1f%%, rms across %.4f' % (bsel.sum(), border.sum(), v[0], v[1], 100 * ev[1] / ev.sum(), np.sqrt(ev[0])))

lr = np.log(np.maximum(rho, 1e-6))
ra, rb = lr[:, 1] - lr[:, 0], lr[:, 2] - lr[:, 0]
rho_base = rho.copy()
if mode == 'line':
    # distance of rho from the line, along its normal (-v2, v1); removed inside U only
    a0, b0 = P.mean(0)
    n_rho = (ra - a0) * -v[1] + (rb - b0) * v[0]
    print('rho across the line in U: p5 %.4f p50 %.4f p95 %.4f' % tuple(np.percentile(n_rho[U], [5, 50, 95])))
    n_rho[~U] = 0
    ra, rb = ra + n_rho * v[1], rb - n_rho * v[0]
    rho_base[:, 1] *= np.exp(n_rho * v[1])
    rho_base[:, 2] *= np.exp(-n_rho * v[0])

# 2. measured error of rho in the pixels with one channel clipped, as a shift t along the line
that = np.zeros(npix); meas = np.zeros(npix, bool)
for cls, dev, div, name in ((2, b - rb, v[1], 'G clipped, from B/R'),
                            (4, a - ra, v[0], 'B clipped, from G/R'),
                            (1, (b - a) - (rb - ra), v[1] - v[0], 'R clipped, from B/G')):
    m = st == cls
    if abs(div) < 0.2 or not m.any():
        print('class %d (%s): %d pixels, not used (divisor %.3f)' % (cls, name, m.sum(), div))
        continue
    that[m] = np.clip(dev[m] / div, -0.5, 0.5); meas |= m
    print('class %d (%s): %d pixels, t p5 %.3f p50 %.3f p95 %.3f' % ((cls, name, m.sum()) + tuple(np.percentile(that[m], [5, 50, 95]))))

# 3. smooth t and fill it into the pixels with two or three channels clipped
unk = np.nonzero(U)[0]
L, _ = laplacian(unk)
A = (lam * L + sp.diags(meas[unk].astype(np.float64))).tocsr()
tu, it, rel = amg_solve(A, that[unk])
t = np.zeros(npix); t[unk] = tu
print('t solve: %d iterations, residual %.1e; t over U p5 %.3f p50 %.3f p95 %.3f; in pixels with 2-3 channels clipped p5 %.3f p50 %.3f p95 %.3f'
      % ((it, rel) + tuple(np.percentile(tu, [5, 50, 95])) + tuple(np.percentile(t[U & ~meas], [5, 50, 95]))))
rho_c = rho_base.copy()
rho_c[U, 1] *= np.exp(t[U] * v[0])
rho_c[U, 2] *= np.exp(t[U] * v[1])

# 4. channel solves with both estimates
sol = {}
for name, hue in (('orig', rho), ('corr-' + mode, rho_c)):
    out = inp.copy()
    for j in range(3):
        unkj, x, it, rel = solve_channel(j, hue)
        out[unkj, j] = x
        if name == 'orig':
            dmax = np.abs(x - pre_c[unkj, j]).max() * inv_clip[j]
            print('channel %d, original rho: %d unknowns, %d iterations, residual %.1e; max |python - C| = %.4f x clip'
                  % (j, len(unkj), it, rel, dmax))
        else:
            r = x / np.maximum(sol['orig'][unkj, j], 1e-6)
            print('channel %d, corrected rho: %d iterations; solution / original solution p5 %.3f p50 %.3f p95 %.3f'
                  % ((j, it) + tuple(np.percentile(r, [5, 50, 95]))))
    sol[name] = out
    g = np.zeros((npix, 4), np.float32); g[:, :3] = out
    with open(os.path.join(outdir, name + '.grid'), 'wb') as fh:
        np.array([w, h], np.int32).tofile(fh)
        g.tofile(fh)

np.savez(os.path.join(outdir, 'proto-%s.npz' % mode), t=t.astype(np.float32).reshape(h, w),
         rho_c=rho_c.astype(np.float32).reshape(h, w, 3),
         orig=sol['orig'].astype(np.float32).reshape(h, w, 3), corr=sol['corr-' + mode].astype(np.float32).reshape(h, w, 3))
print('done in %.0f s' % (time.time() - t0))

# lift.py <workdir>...: issue 29 (opposed-brightness), offline prototype on the linear renders.
# Writes <LIFT_OUT>.pfm next to <LIFT_SRC>.pfm (default opposed), so that pair_align.py, deficit and
# crop_sheet.py treat it as one more method.
# Inpaint opposed leaves fully clipped pixels near the clipping level, 0.2 EV (median) below the truth.
# Per fully clipped region (8-connected; a render pixel counts when all three channels have at least
# half of their photosites clipped, masks.npz F, as dome.py), the luminance is refilled from
# everything outside the region, which is fixed:
#   LIFT_ORDER=1  flat lift: minimizes the squared gradient (Laplace solve): the rim's level, no rise
#                 above it (maximum principle)
#   LIFT_ORDER=2  dome: minimizes the squared Laplacian (biharmonic, salvage/dome.py), which continues
#                 the rim's slopes, so the core can rise above the rim
#   LIFT_LOG=1    the solve on log luminance instead of linear
# Where the fill is brighter, the pixel's RGB is scaled up (hue kept), at most LIFT_MAX_EV; nothing is
# darkened. Regions above LIFT_MAX_PX render pixels are left alone (0: no limit). Regions with more
# than LIFT_NMAX pixels are solved iteratively (CG with an AMG preconditioner) instead of directly.
import sys, os, json, time
import numpy as np, cv2
import scipy.sparse as sp
import scipy.sparse.linalg as spl
import pyamg
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

SRC = os.environ.get('LIFT_SRC', 'opposed')
OUT = os.environ.get('LIFT_OUT', 'opp-lift')
ORDER = int(os.environ.get('LIFT_ORDER', '2'))
LOG = os.environ.get('LIFT_LOG') == '1'
MAX_PX = int(os.environ.get('LIFT_MAX_PX', '0'))
MAX_EV = float(os.environ.get('LIFT_MAX_EV', '4'))
NMAX = int(os.environ.get('LIFT_NMAX', '40000'))
# LIFT_COARSE=1: regions above NMAX on the coarse grid instead of AMG at full size (inaccurate for
# the dome: the rim's slope is averaged away; t60: median 0.45 log units off at a factor of 3)
COARSE = os.environ.get('LIFT_COARSE') == '1'
# LIFT_ROBUST=tau: pixels outside the region darker than tau times the region's median luminance
# (occluders: trees, roofs against a clipped sky) are left free instead of fixed, so that their
# dark-to-bright step is not extrapolated into a dome (t25 sky: median x6.4 without)
ROBUST = float(os.environ.get('LIFT_ROBUST', '0'))
# LIFT_SOFT=H: the rise is compressed to H tanh(rise / H), in EV, instead of the hard LIFT_MAX_EV cap
SOFT = float(os.environ.get('LIFT_SOFT', '0'))


def write_pfm(path, a):
    # inverse of pfm.read_pfm: little endian, rows stored bottom to top
    h, w = a.shape[:2]
    with open(path, 'wb') as f:
        f.write(b'PF\n%d %d\n-1.0\n' % (w, h))
        np.ascontiguousarray(a[::-1], '<f4').tofile(f)


def operator(h, w):
    # rows of the penalized term on the crop: order 1 one row per horizontal and vertical neighbor
    # pair (forward differences), order 2 the 5-point Laplacian with only the in-crop neighbors
    n = h * w
    idx = np.arange(n).reshape(h, w)
    if ORDER == 1:
        a = np.concatenate([idx[:, :-1].ravel(), idx[:-1, :].ravel()])
        b = np.concatenate([idx[:, 1:].ravel(), idx[1:, :].ravel()])
        m = a.size
        r = np.arange(m)
        return sp.csr_matrix((np.concatenate([np.ones(m), -np.ones(m)]), (np.concatenate([r, r]), np.concatenate([a, b]))),
                             shape=(m, n))
    rows, cols, vals = [], [], []
    for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        src = idx[max(0, -dy):h - max(0, dy), max(0, -dx):w - max(0, dx)].ravel()
        dst = idx[max(0, dy):h - max(0, -dy) or None, max(0, dx):w - max(0, -dx) or None].ravel()
        rows += [src, src]
        cols += [dst, src]
        vals += [np.ones(src.size), -np.ones(src.size)]
    return sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))), shape=(n, n))


def solve(L, reg, free=None):
    # fills L inside reg from the fixed values outside it; returns the filled crop. Pixels in free
    # are unknowns too (natural boundary: they exert no slope on the region), tied to their own
    # values by a tiny weight only, so that a component without fixed pixels stays solvable
    ch, cw = reg.shape
    D = operator(ch, cw)
    unk = reg if free is None else reg | free
    u = np.flatnonzero(unk.ravel())
    k = np.flatnonzero(~unk.ravel())
    Du, Dk = D[:, u], D[:, k]
    A = (Du.T @ Du).tocsc()
    b = -(Du.T @ (Dk @ L.ravel()[k]))
    if free is not None:
        tie = 1e-6 * free.ravel()[u]
        A = (A + sp.diags(tie)).tocsc()
        b = b + tie * L.ravel()[u]
    out = L.ravel().copy()
    if u.size <= NMAX:
        out[u] = spl.spsolve(A, b)
    else:
        # large regions (skies): smoothed aggregation AMG as a CG preconditioner, started from the
        # region's current values
        ml = pyamg.smoothed_aggregation_solver(A.tocsr(), max_coarse=500)
        out[u] = ml.solve(b, x0=L.ravel()[u].astype(np.float64), tol=1e-8, accel='cg', maxiter=300)
    return out.reshape(ch, cw)


def lift(img, full):
    out = img.copy()
    Y = np.maximum(ev2.lum(np.maximum(img, 0)), 1e-6)
    L = np.log(Y) if LOG else Y
    n, lab, st, _ = cv2.connectedComponentsWithStats(full.astype(np.uint8), 8)
    H, W = full.shape
    done = skipped = 0
    for i in range(1, n):
        x, y, w, h, area = st[i]
        if MAX_PX and area > MAX_PX:
            skipped += 1
            continue
        # margin: 2 px suffice for the stencils; more gives the coarse grid whole cells of context
        f = int(np.ceil(np.sqrt(area / NMAX))) if COARSE and area > NMAX else 1
        m = 4 * f
        y0, y1, x0, x1 = max(0, y - m), min(H, y + h + m), max(0, x - m), min(W, x + w + m)
        reg = lab[y0:y1, x0:x1] == i
        Lc = L[y0:y1, x0:x1]
        if f == 1:
            free = None
            if ROBUST:
                Yc = Y[y0:y1, x0:x1]
                free = ~reg & (Yc < ROBUST * np.median(Yc[reg]))
            sol = solve(Lc, reg, free)
        else:
            # coarse grid: a cell is unknown if any of its pixels is in the region, else it holds the
            # mean of its known pixels (cells cut by the crop edge are averaged over what they hold)
            ch, cw = reg.shape
            gh, gw = -(-ch // f), -(-cw // f)
            pad = lambda a, v: np.pad(a, ((0, gh * f - ch), (0, gw * f - cw)), constant_values=v)
            kn = pad((~reg).astype(np.float64), 0).reshape(gh, f, gw, f)
            cnt = pad(np.ones_like(Lc, np.float64), 0).reshape(gh, f, gw, f).sum((1, 3))
            sm = (pad(Lc.astype(np.float64), 0).reshape(gh, f, gw, f) * kn).sum((1, 3))
            nk = kn.sum((1, 3))
            creg = nk < cnt
            cL = np.where(creg, 0, sm / np.maximum(nk, 1))
            csol = solve(cL, creg)
            # bilinear back to the crop at cell centers
            sol = cv2.resize(csol.astype(np.float32), (gw * f, gh * f), interpolation=cv2.INTER_LINEAR)[:ch, :cw]
        cur = Lc[reg]
        new = sol[reg]
        rise = np.log(np.maximum(new, 1e-6) / cur) if not LOG else new - cur
        if SOFT:
            rise = SOFT * np.log(2) * np.tanh(np.maximum(rise, 0) / (SOFT * np.log(2)))
        else:
            rise = np.clip(rise, 0, MAX_EV * np.log(2))
        g = np.ones(reg.shape, np.float32)
        g[reg] = np.exp(rise)
        out[y0:y1, x0:x1] *= g[..., None]
        done += 1
    return out, done, skipped


def main(wd):
    t = time.time()
    img = ev2.read_pfm(os.path.join(wd, SRC + '.pfm')).astype(np.float32)
    F = np.load(os.path.join(wd, 'masks.npz'))['F'].astype(np.float32)
    full = F.min(2) >= 0.5
    # LIFT_MASK=any: fill every clipped pixel (any channel, partly clipped too) from the unclipped
    # surroundings. On lamps opposed's partly clipped rim is itself too dark (g294: 0.4-0.5 EV below
    # the truth where one channel clips), so a fill from that rim cannot rise far enough
    sel = (F.max(2) > 0) if os.environ.get('LIFT_MASK') == 'any' else full
    out, done, skipped = lift(img, sel)
    dst = os.path.join(wd, OUT + '.pfm')
    if os.path.exists(dst):
        os.remove(dst)
    write_pfm(dst, out.astype(np.float32))
    rose = full & (out.max(2) > img.max(2) * 1.01)
    print('    clipped px raised: %.1f%%' % (100 * (sel & (out.max(2) > img.max(2) * 1.01)).sum() / max(sel.sum(), 1)), end='')
    gain = ev2.lum(out[full]) / np.maximum(ev2.lum(img[full]), 1e-6) if full.any() else np.ones(1)
    print('%-44s %4d regions, %d skipped; %5.1f%% of fully clipped px raised; gain median x%.2f, p90 x%.2f, max x%.2f; %.1f s' % (
        os.path.basename(wd.rstrip('/'))[:44], done, skipped, 100 * rose.sum() / max(full.sum(), 1),
        np.median(gain), np.percentile(gain, 90), gain.max(), time.time() - t), flush=True)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)

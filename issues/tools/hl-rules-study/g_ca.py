# g_ca.py <dump root> <plan.json> <names> [-o out.json]
# Diagnostic G of the rules study: does lateral chromatic aberration (CA) tint the border
# of clipped regions, and the hue estimate built from it?
#
# Lateral CA scales the red and blue images radially against green. At an edge, the tint
# it adds depends on the sign of s = n . r, where n is the unit direction from the dark
# side to the bright side of the edge and r the radial direction from the image center:
# the sign flips between edges facing the center and edges facing away, and so between
# opposite sides of the frame for the same kind of edge. The magnitude grows with the
# distance from the center. So if CA contributes, the tint of border pixels correlates
# with s x rad (rad = distance from the center over the half diagonal), and with opposite
# signs on the two sides of the frame center.
#
# On the reduced-resolution dumps of f_dump.py (pack.npz), hue estimate of "hue from
# measured channels, hue estimate in fully clipped" (rho7):
#   cause   per border pixel B (unflagged, 4-adjacent to a clipped pixel) with all three
#           channels above 5% of clipping: tint (ln R/G, ln B/G) minus its median over the
#           image's border pixels; n from the gradient of ln(sum of channels, clipped at
#           the clipping levels) smoothed by sigma 2; edges with |grad| below its median
#           are left out. Pearson correlation and least-squares slope of each tint against
#           s x rad, overall and separately for the left and right half of the frame
#           (x < center, x > center), where the same scene edge orientation faces
#           opposite ways
#   effect  per clipped pixel at grid distance 2..20 from the border, the error of rho7
#           against the truth, (ln rho_R/rho_G - ln t_R/t_G, same for B), against s x rad
#           of its nearest border pixel; the same statistics
# The truth is that of f_rule.py (truth_grid), so pairs carry the same exclusions.
import sys, os, json
import numpy as np, cv2
from scipy import ndimage
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f_rule import unpack, truth_grid


def stats(x, y, nx=None, ny=None):
    """Correlation and slope of y on x; with nx, ny also the slope of x in a regression
    y ~ x + nx + ny + 1, which removes tints that follow the edge orientation in image
    axes (such as the offsets of the Bayer samples within a 2x2 block) rather than the
    radial direction."""
    k = np.isfinite(x) & np.isfinite(y)
    if nx is not None:
        k &= np.isfinite(nx) & np.isfinite(ny)
    x, y = x[k], y[k]
    if len(x) < 100 or x.std() < 1e-9:
        return None
    c = float(np.corrcoef(x, y)[0, 1])
    sl = float(np.polyfit(x, y, 1)[0])
    out = dict(n=int(len(x)), corr=round(c, 3), slope=round(sl, 4))
    if nx is not None:
        X = np.stack([x, nx[k], ny[k], np.ones_like(x)], 1)
        beta, res_, _, _ = np.linalg.lstsq(X, y, rcond=None)
        r = y - X @ beta
        # standard error of the radial slope
        cov = np.linalg.inv(X.T @ X) * (r @ r) / max(len(y) - 4, 1)
        out.update(slope_ctrl=round(float(beta[0]), 4), se_ctrl=round(float(np.sqrt(cov[0, 0])), 4),
                   slope_nx=round(float(beta[1]), 4), slope_ny=round(float(beta[2]), 4))
    return out


def main():
    a = sys.argv[1:]
    root, plan, names = a[0], json.load(open(a[1])), a[2].split(',')
    outp = a[a.index('-o') + 1] if '-o' in a else None
    res = {}
    for e in plan:
        if e['name'] not in names:
            continue
        wd = os.path.join(root, e['name'])
        if not os.path.exists(os.path.join(wd, 'pack.npz')):
            continue
        x7, x10, _, tru = unpack(wd)
        v, st, T = x7['in'], x7['state'], x7['T']
        h, w = st.shape
        t, excl, _ = truth_grid(x7, tru, e['k'], e['kind'])
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
        cy, cx = (h - 1) / 2, (w - 1) / 2
        half = np.hypot(cy, cx)
        ry, rx = (yy - cy) / half, (xx - cx) / half
        rad = np.hypot(ry, rx)
        lb = np.log(np.maximum(np.minimum(v, T).sum(-1), 1e-6)).astype(np.float32)
        lb = cv2.GaussianBlur(lb, (0, 0), 2)
        gy, gx = np.gradient(lb)
        gm = np.hypot(gx, gy)
        s = (gx * rx + gy * ry) / np.maximum(gm * rad, 1e-9)  # cos between n and radial
        sr = s * rad
        nxx, nyy = gx / np.maximum(gm, 1e-9), gy / np.maximum(gm, 1e-9)
        border = x10['border'] > 0
        ok = border & (v >= 0.05 * T).all(-1) & (gm > np.median(gm[border]))
        tint = np.stack([np.log(np.maximum(v[..., 0], 1e-6) / np.maximum(v[..., 1], 1e-6)),
                         np.log(np.maximum(v[..., 2], 1e-6) / np.maximum(v[..., 1], 1e-6))], -1)
        r = dict()
        for name, half_ in (('all', np.ones_like(ok)), ('left', xx < cx), ('right', xx > cx), ('top', yy < cy), ('bottom', yy > cy)):
            q = ok & half_
            for c, cn in ((0, 'R'), (1, 'B')):
                tt = tint[..., c][q]
                r['cause|%s|%s' % (name, cn)] = stats(sr[q], tt - np.median(tt) if len(tt) else tt, nxx[q], nyy[q])
        # effect: rho error inside U against the nearest border pixel's s x rad
        d, (iy, ix) = ndimage.distance_transform_edt(~border, return_indices=True)
        U = (st > 0) & (d >= 2) & (d <= 20) & ~excl & (t.min(-1) > 0)
        rho = np.maximum(x7['rho'], 1e-6)
        t = np.maximum(t, 1e-9)
        err = np.stack([np.log(rho[..., 0] / rho[..., 1]) - np.log(t[..., 0] / np.maximum(t[..., 1], 1e-9)),
                        np.log(rho[..., 2] / rho[..., 1]) - np.log(t[..., 2] / np.maximum(t[..., 1], 1e-9))], -1)
        srn = sr[iy, ix]
        okn = ok[iy, ix]
        for name, half_ in (('all', np.ones_like(U)), ('left', xx < cx), ('right', xx > cx), ('top', yy < cy), ('bottom', yy > cy)):
            q = U & okn & half_
            for c, cn in ((0, 'R'), (1, 'B')):
                r['effect|%s|%s' % (name, cn)] = stats(srn[q], err[..., c][q], nxx[iy, ix][q], nyy[iy, ix][q])
        res[e['name']] = r
        print(e['name'], json.dumps(r), flush=True)
    if outp:
        json.dump(res, open(outp, 'w'), indent=1)


if __name__ == '__main__':
    main()

# g_shift.py <plan.json> <names> [-o out.json]
# Direct measurement of lateral chromatic aberration for diagnostic G: per image, the
# shift of the red and blue photosite planes against the first green plane, measured by
# phase correlation in tiles of 96 x 96 quads (192 photosites), fitted by a radial model
#     shift = a (p - center) + c
# (a: magnification difference, c: the fixed offset of the color's sites in the 2x2
# block, and any constant misregistration). The second green plane is a control: it has
# the same optics, so its a must come out near 0. Reported: a times the half diagonal,
# the CA displacement at the frame corner, in photosites, with its standard error.
# Uses the clipped frame of pairs (brighter, so less noisy) and the frame of synthetic
# entries; tiles with any photosite above 90% of the white level, a mean below 2%, low
# texture or a weak correlation peak are left out. Correlation on linear values.
import sys, os, json
import numpy as np, cv2, rawpy
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
from bracket_scan import white_level
from ev2 import model

TILE = 96


def planes(path):
    cam = model(path)
    with rawpy.imread(path) as r:
        a = r.raw_image_visible.astype(np.float32)
        cols = r.raw_colors_visible
        black = np.array(r.black_level_per_channel, np.float32)
        white = white_level(cam, r)
        a = (a - black[cols]) / (white - black[cols])
        h, w = (a.shape[0] // 2) * 2, (a.shape[1] // 2) * 2
        P = {}
        for dy in (0, 1):
            for dx in (0, 1):
                c = int(cols[dy, dx])
                key = {0: 'R', 1: 'G1', 2: 'B', 3: 'G2'}[c]
                if key == 'G1' and 'G1' in P:
                    key = 'G2'
                P[key] = a[dy:h:2, dx:w:2]
    return P


def fit(pts, sh):
    """Least squares of shift = a (p - center) + c, x and y together; returns a, se(a)."""
    n = len(pts)
    X = np.zeros((2 * n, 3))
    X[:n, 0] = pts[:, 0]
    X[n:, 0] = pts[:, 1]
    X[:n, 1] = 1
    X[n:, 2] = 1
    y = np.concatenate([sh[:, 0], sh[:, 1]])
    beta, _, _, _ = np.linalg.lstsq(X, y, rcond=None)
    r = y - X @ beta
    cov = np.linalg.inv(X.T @ X) * (r @ r) / max(2 * n - 3, 1)
    return float(beta[0]), float(np.sqrt(cov[0, 0])), float(np.sqrt((r ** 2).mean()))


def main():
    a = sys.argv[1:]
    plan, names = json.load(open(a[0])), a[1].split(',')
    outp = a[a.index('-o') + 1] if '-o' in a else None
    res = {}
    for e in plan:
        if e['name'] not in names:
            continue
        P = planes(e['clipped'])
        g = P['G1']
        h, w = g.shape
        cy, cx = (h - 1) / 2, (w - 1) / 2
        half = np.hypot(cy, cx)
        win = cv2.createHanningWindow((TILE, TILE), cv2.CV_32F)
        out = {}
        for key in ('R', 'B', 'G2'):
            q = P[key]
            pts, sh = [], []
            for y0 in range(0, h - TILE + 1, TILE):
                for x0 in range(0, w - TILE + 1, TILE):
                    tg = g[y0:y0 + TILE, x0:x0 + TILE]
                    tq = q[y0:y0 + TILE, x0:x0 + TILE]
                    if tg.max() > 0.9 or tq.max() > 0.9 or tg.mean() < 0.02:
                        continue
                    lg = (tg / tg.mean()).astype(np.float32)
                    lq = (tq / max(float(tq.mean()), 1e-6)).astype(np.float32)
                    gm = np.hypot(*np.gradient(lg))
                    if np.percentile(gm, 90) < 0.05:
                        continue
                    (sx, sy), resp = cv2.phaseCorrelate(lg - lg.mean(), lq - lq.mean(), win)
                    if resp < 0.2 or abs(sx) > 3 or abs(sy) > 3:
                        continue
                    pts.append((x0 + TILE / 2 - cx, y0 + TILE / 2 - cy))
                    sh.append((sx, sy))
            if len(pts) < 10:
                out[key] = None
                continue
            pts, sh = np.array(pts), np.array(sh)
            k, se, rms = fit(pts, sh)
            # quads to photosites: x2
            out[key] = dict(tiles=len(pts), corner_px=round(2 * k * half, 3), se_px=round(2 * se * half, 3),
                            rms_px=round(2 * rms, 3))
        res[e['name']] = out
        print(e['name'], json.dumps(out), flush=True)
    if outp:
        json.dump(res, open(outp, 'w'), indent=1)


if __name__ == '__main__':
    main()

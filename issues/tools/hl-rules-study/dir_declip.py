# dir_declip.py <workdir>...: issue directional-declipping, step 1. Does disagreement between 1D
# reconstructions along four directions predict where inpaint opposed and gradient domain are wrong?
# Bayer raws only (all pairs of issue 2 are Bayer).
# 1. Per CFA plane of the clipped frame (normalized as ev2.raw_flags(), clipped at 0.98) and per
#    direction (row, column, both diagonals of the plane), every clipped run of at most RUNMAX
#    samples with two unclipped samples on each side gets a least-squares quadratic through those
#    four samples, evaluated inside the run and raised to the clipping level (a clipped value is at
#    least that). This continues the slopes on both sides, as the directional article proposes.
# 2. Disagreement per plane sample: standard deviation of the log estimates over the directions that
#    have one (at least 2); per 2x2 quad the largest over its planes. Mapped onto the render with
#    ev2.register(), as pair_align.py maps the truth's clipping.
# 3. Per render pixel that is clipped (masks.npz F), has a truth (pair_align.py's noTruth) and is not
#    dark in the truth (Y >= 0.05): error of each method against the aligned truth, |log2 Y ratio| and
#    u'v' distance. Spearman correlation of the disagreement with each error, and of the trivial
#    predictor "distance to the nearest unclipped pixel" for comparison; also the correlation of the
#    disagreement within distance bands (does it add anything beyond distance?).
# Writes dir_declip.json per workdir and prints one line per pair.
import sys, os, json
import numpy as np, cv2, rawpy
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

RUNMAX = int(os.environ.get('DD_RUNMAX', '40'))
METHODS = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')
BANDS = [(0, 3), (3, 8), (8, 20), (20, 1e9)]
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)


def lines(h, w, direction):
    idx = np.arange(h * w).reshape(h, w)
    if direction == 0:
        ls = list(idx)
    elif direction == 1:
        ls = list(idx.T)
    elif direction == 2:
        ls = [np.diagonal(idx, k) for k in range(-h + 1, w)]
    else:
        f = idx[:, ::-1]
        ls = [np.diagonal(f, k) for k in range(-h + 1, w)]
    # one long index array, lines separated by -1
    out = np.full(sum(len(l) + 1 for l in ls), -1, np.int64)
    p = 0
    for l in ls:
        out[p:p + len(l)] = l
        p += len(l) + 1
    return out


def estimate(plane, clip, direction):
    h, w = plane.shape
    li = lines(h, w, direction)
    valid = li >= 0
    v = np.where(valid, plane.ravel()[np.maximum(li, 0)], 0.0)
    c = np.where(valid, clip.ravel()[np.maximum(li, 0)], False)
    # runs of clipped samples
    d = np.diff(np.concatenate([[0], c.astype(np.int8), [0]]))
    a, b = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    L = b - a
    ok = (L <= RUNMAX) & (a >= 2) & (b + 1 < len(li))
    a, b, L = a[ok], b[ok], L[ok]
    nb = np.stack([a - 2, a - 1, b, b + 1], 1)
    good = (valid[nb] & ~c[nb]).all(1)
    a, b, L, nb = a[good], b[good], L[good], nb[good]
    est = np.full(h * w, np.nan, np.float32)
    if not len(a):
        return est.reshape(h, w)
    # quadratic in x = position - a through the four neighbors, least squares, batched
    x = np.stack([-2 * np.ones_like(L), -np.ones_like(L), L, L + 1], 1).astype(np.float64)
    y = v[nb].astype(np.float64)
    X = np.stack([np.ones_like(x), x, x * x], 2)
    coef = np.linalg.solve(np.einsum('nki,nkj->nij', X, X) + 1e-9 * np.eye(3), np.einsum('nki,nk->ni', X, y)[..., None])[..., 0]
    run = np.repeat(np.arange(len(a)), L)
    off = np.arange(L.sum()) - np.repeat(np.cumsum(L) - L, L)
    val = coef[run, 0] + coef[run, 1] * off + coef[run, 2] * off * off
    est[li[a[run] + off]] = np.maximum(val, 1.0)
    return est.reshape(h, w)


def disagreement(path):
    cam = ev2.model(path)
    with rawpy.imread(path) as r:
        a = r.raw_image_visible.astype(np.float32)
        cols = r.raw_colors_visible
        black = np.array(r.black_level_per_channel, np.float32)
        white = ev2.white_level(cam, r)
        a = (a - black[cols]) / (white - black[cols])
    h, w = (a.shape[0] // 2) * 2, (a.shape[1] // 2) * 2
    a = a[:h, :w]
    D = np.full((h // 2, w // 2), np.nan, np.float32)
    for dy in (0, 1):
        for dx in (0, 1):
            p = a[dy::2, dx::2]
            clip = p >= 0.98
            E = np.stack([estimate(p, clip, k) for k in range(4)])
            n = np.isfinite(E).sum(0)
            sd = np.nanstd(np.log(np.where(np.isfinite(E), E, np.nan)), 0) if n.any() else np.full(p.shape, np.nan)
            sd = np.where(n >= 2, sd, np.nan)
            D = np.fmax(D, sd)
    return D


def spearman(x, y):
    if len(x) < 50:
        return None
    rx, ry = np.argsort(np.argsort(x)), np.argsort(np.argsort(y))
    return float(np.corrcoef(rx, ry)[0, 1])


def main(wd):
    name = os.path.basename(wd.rstrip('/'))
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    s = 2.0 ** (sc['k'] - ev2.E0)
    N = ev2.read_pfm(os.path.join(wd, 'none.pfm')).astype(np.float32) * s
    D = disagreement(sc['clipped'])
    _, g, _ = ev2.raw_flags(sc['clipped'], 1.0)
    apply, _ = ev2.register(g, ev2.lum(np.maximum(N, 0)))
    have = apply(np.isfinite(D).astype(np.float32)) > 0.5
    Dr = apply(np.nan_to_num(D, nan=0.0))
    al = os.path.join(wd, 'align')
    ta = cv2.imread(os.path.join(al, 'al_0001.tif'), cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]),
                    cv2.IMREAD_UNCHANGED)
    valid = ta[..., 3] > 0 if ta.shape[2] == 4 else np.ones(ta.shape[:2], bool)
    T = cv2.cvtColor(ta[..., :3].astype(np.float32), cv2.COLOR_BGR2RGB)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5:
        Ra = Ra / np.iinfo(mk.dtype).max
    noTruth = cv2.dilate(((Ra > 0.02) | ~valid).astype(np.uint8), disk(5)) > 0
    clip = np.load(os.path.join(wd, 'masks.npz'))['F'].astype(np.float32).max(2) > 0
    Yt = ev2.lum(np.maximum(T, 0))
    sel = clip & ~noTruth & (Yt >= 0.05) & have
    dist = cv2.distanceTransform(clip.astype(np.uint8), cv2.DIST_L2, 5)
    res = dict(pixels=int(sel.sum()), coverage=float((clip & ~noTruth & (Yt >= 0.05) & have).sum() / max(1, (clip & ~noTruth & (Yt >= 0.05)).sum())))
    uvT = ev2.uv(np.maximum(T, 0))
    for m in METHODS:
        A = ev2.read_pfm(os.path.join(wd, m + '.pfm')).astype(np.float32) * s
        eY = np.abs(np.log2(np.maximum(ev2.lum(np.maximum(A, 0)), 1e-6) / np.maximum(Yt, 1e-6)))[sel]
        eC = np.hypot(*(ev2.uv(np.maximum(A, 0)) - uvT)[sel].T)
        for en, e in (('Y', eY), ('uv', eC)):
            res['%s %s dis' % (m, en)] = spearman(Dr[sel], e)
            res['%s %s dist' % (m, en)] = spearman(dist[sel], e)
            within = [spearman(Dr[sel][(dist[sel] >= lo) & (dist[sel] < hi)], e[(dist[sel] >= lo) & (dist[sel] < hi)])
                      for lo, hi in BANDS]
            within = [x for x in within if x is not None]
            res['%s %s dis|dist' % (m, en)] = float(np.mean(within)) if within else None
    json.dump(res, open(os.path.join(wd, 'dir_declip%s.json' % os.environ.get('DD_SUFFIX', '')), 'w'), indent=1)
    f = lambda v: '  -  ' if v is None else '%+.2f' % v
    print('%-40s px %7d cov %3.0f%%  ' % (name[:40], res['pixels'], 100 * res['coverage'])
          + '  '.join('%s %s: dis %s dist %s dis|dist %s' % (m[:3], en, f(res['%s %s dis' % (m, en)]), f(res['%s %s dist' % (m, en)]),
                                                         f(res['%s %s dis|dist' % (m, en)])) for m in METHODS for en in ('Y', 'uv')),
          flush=True)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)

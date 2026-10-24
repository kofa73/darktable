# border_local.py <workdir>...: local version of border_check.py (issue border-classification).
# Question: are gradient domain's color errors in clipped regions concentrated next to occluder
# borders (dark beyond the edge), as freq-domain2.md fix 1 assumes? Then making occluder segments
# zero-flux should help.
# Per clipped region of at least MIN_PX pixels (as border_check.py): border pixels (ring 2 px outside)
# are classified by the darkest pixel within 2 px beyond them in the clipped frame's own render:
# bright (>= 0.75 of the clip level), occluder (< 0.15), or mid. Each interior pixel with a truth
# (pair_align.py's noTruth left out, truth Y >= 0.05) takes the class of
# its nearest border pixel (distance transform with labels), and is split into partly and fully
# clipped (all three channels flagged in its quad, masks.npz F). Per class and distance band: median
# u'v' error against the aligned truth of inpaint opposed and gradient domain, and of the nearest
# border pixel's own color (what a fill that copied it inward would give). Pixels are pooled over all
# regions of all workdirs; prints one table and writes border_local.json next to the first workdir's
# parent (the run directory).
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

MIN_PX = int(os.environ.get('BC_MIN_PX', '5000'))
DMAX = 1e9
BANDS = [(2, 10), (10, 25), (25, 60), (60, 150), (150, 100000)]
CLASSES = ('bright', 'mid', 'occluder')
METHODS = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)


def uvimg(rgb):
    return ev2.uv(np.maximum(rgb, 0))


def collect(wd, pool):
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    kind = sc['ref'].split('/bracketed/')[1].split('/')[0] if '/bracketed/' in sc['ref'] else '?'
    s = 2.0 ** (sc['k'] - ev2.E0)
    al = os.path.join(wd, 'align')
    N = ev2.read_pfm(os.path.join(wd, 'none.pfm')).astype(np.float32) * s
    imgs = {m: ev2.read_pfm(os.path.join(wd, m + '.pfm')).astype(np.float32) * s for m in METHODS}
    ta = cv2.imread(os.path.join(al, 'al_0001.tif'), cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]),
                    cv2.IMREAD_UNCHANGED)
    valid = ta[..., 3] > 0 if ta.shape[2] == 4 else np.ones(ta.shape[:2], bool)
    T = cv2.cvtColor(ta[..., :3].astype(np.float32), cv2.COLOR_BGR2RGB)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5:
        Ra = Ra / np.iinfo(mk.dtype).max
    noTruth = cv2.dilate(((Ra > 0.02) | ~valid).astype(np.uint8), disk(5)) > 0
    F = np.load(os.path.join(wd, 'masks.npz'))['F'].astype(np.float32)
    clip = F.max(2) > 0
    # all three channels clipped in (part of) the quad: gradient domain's fully clipped fill
    full = F.min(2) > 0
    Y, Yt = ev2.lum(np.maximum(N, 0)), ev2.lum(np.maximum(T, 0))
    uvN, uvT = uvimg(N), uvimg(T)
    uvM = {m: uvimg(imgs[m]) for m in METHODS}
    n, lab, st, _ = cv2.connectedComponentsWithStats(clip.astype(np.uint8), 8)
    H, W = clip.shape
    for i in range(1, n):
        x, y, w, h, area = st[i]
        if area < MIN_PX:
            continue
        y0, y1, x0, x1 = max(0, y - 6), min(H, y + h + 6), max(0, x - 6), min(W, x + w + 6)
        sl = np.s_[y0:y1, x0:x1]
        r = (lab[sl] == i).astype(np.uint8)
        border = (cv2.dilate(r, disk(2)) > 0) & ~(cv2.dilate(r, disk(1)) > 0)
        dark = cv2.erode(np.where(r > 0, np.float32(1e3), Y[sl]), disk(2))
        cls = np.full(r.shape, -1, np.int8)
        cls[border & (dark >= 0.75)] = 0
        cls[border & (dark >= 0.15) & (dark < 0.75)] = 1
        cls[border & (dark < 0.15)] = 2
        # nearest border pixel of every pixel: distance transform to the border pixels (zeros)
        dist, lbl = cv2.distanceTransformWithLabels((~border).astype(np.uint8), cv2.DIST_L2, 5,
                                                    labelType=cv2.DIST_LABEL_PIXEL)
        by, bx = np.nonzero(border)
        # labels number the zero pixels in row-major scan order, starting at 1
        order = np.zeros(lbl.max() + 1, np.int64)
        zy, zx = np.nonzero(lbl * border.astype(np.int32) > 0)
        order[lbl[zy, zx]] = zy * r.shape[1] + zx
        inner = (r > 0) & ~noTruth[sl] & (Yt[sl] >= 0.05) & (dist <= DMAX)
        iy, ix = np.nonzero(inner)
        nb = order[lbl[iy, ix]]
        ny, nx = nb // r.shape[1], nb % r.shape[1]
        c = cls[ny, nx]
        d = dist[iy, ix]
        gy, gx = iy + y0, ix + x0
        t = uvT[gy, gx]
        e = {m: np.hypot(*(uvM[m][gy, gx] - t).T) for m in METHODS}
        e['border'] = np.hypot(*(uvN[ny + y0, nx + x0] - t).T)
        pool.append(dict(kind=kind, cls=c, d=d, err=e, full=full[gy, gx], region=(os.path.basename(wd.rstrip('/')), i)))


def main():
    pool = []
    for wd in sys.argv[1:]:
        collect(wd, pool)
        print('.', end='', flush=True)
    print()
    cat = lambda k: np.concatenate([p[k] for p in pool])
    cls, d, full = cat('cls'), cat('d'), cat('full')
    err = {m: np.concatenate([p['err'][m] for p in pool]) for m in METHODS + ['border']}
    kinds = np.concatenate([np.full(len(p['d']), p['kind']) for p in pool])
    out = {}
    for kname, km in [('all', np.ones(len(d), bool))] + [(k, kinds == k) for k in sorted(set(kinds))]:
        print('== %s: median u\'v\' error by class of the nearest border pixel and distance (px), n pixels' % kname)
        for fn, fm in (('partial', ~full), ('full', full)):
            for ci, cn in enumerate(CLASSES):
                for a, b in BANDS:
                    m = km & fm & (cls == ci) & (d >= a) & (d < b)
                    if m.sum() < 500:
                        continue
                    v = {k: [float(x) for x in np.percentile(err[k][m], [50, 90])] for k in err}
                    out['%s %s %s %d-%d' % (kname, fn, cn, a, b)] = dict(n=int(m.sum()), **v)
                    print('  %-7s %-8s %3d-%-6d px  n %8d  ' % (fn, cn, a, b, m.sum())
                          + '  '.join('%s %.4f/%.4f' % (k[:6], *v[k]) for k in METHODS + ['border']))
    json.dump(out, open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(sys.argv[1]))), 'border_local.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()

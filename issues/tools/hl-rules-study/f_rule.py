# f_rule.py <dump root> <plan.json> <names | all> [-o out.json]
# f_rule.py --pack <image dump dir>...: pack the dumps (pack.npz) and delete them
# Phase 3 of the rules study: rule F (pooled hue candidates with a feasibility test)
# against the hue estimates of the method, on dumps made by f_dump.py (reduced
# resolution grid of _gd_solve(): in.f32 the converted input, state.u8 the flags,
# rho.f32 the hue estimate before the fills, donor.u8 / border.u8 of the continuous
# border, meta.txt the clipping levels).
#
# Truth: the truth frame's converted input, brought into the clipped frame's white
# balance by the ratio of the clipping levels (they are 0.98 x processed_maximum, which
# follows the as-shot coefficients) and raised by k EV. For pairs it is shifted onto
# the clipped frame by phase correlation, and pixels near strong edges are left out, as
# in ev2.py. Pixels the truth frame clips itself (dilated by 1) are left out.
#
# Errors, in degrees, over the partly clipped (state 1..6) and fully clipped (7) pixels:
#   rho      angle between the hue estimate and the truth
#   implied  angle between the initial guess of the channel solves (see implied()) and
#            the truth: an approximation of how rho enters the solves, not the solve
#
# Rule F, as implemented here:
#   tiles      3 x 3 over the grid
#   candidates per tile: pixels with no flagged channel, max_c v_c / T_c >= 0.5 (near
#              clipping), smooth (each 4-neighbor's channel sum within 5% and unflagged),
#              clustered by k-means into at most 4 clusters on (ln R/G, ln B/G); clusters
#              under 5% of the tile's candidates are dropped; a tile with fewer than 50
#              candidates has no pool. Candidate hue = exp of the cluster center
#   pieces     a 4-connected region of U split by the tiles
#   feasibility of candidate h in a partly clipped pixel: for every flagged k and
#              measured m with v_m >= 0.05 T_m: v_m h_k / h_m >= T_k (1 - 0.02)
#   match      mean |ln(v_a / v_b) - ln(h_a / h_b)| over the pixels with two measured
#              channels, both >= 0.05 T
#   choice     among the candidates of the piece's tile (else the 8 neighbor tiles, else
#              all tiles) whose feasible share is within 0.05 of the best, the best match;
#              without two-channel pixels, the most feasible, ties to the larger cluster.
#              A piece without partly clipped pixels, or with no pool, keeps the hue
#              estimate.
#   F everywhere: every piece takes its candidate, as a constant hue
#   F gated on 7 (10), 2+ flags: as F gated on 7 (10), in pixels with two or three
#              flagged channels only
#   F gated: only regions whose distinct border pixels are fewer than 50% donors of the
#              continuous border (x10 dump) take F; the others keep the base estimate
import sys, os, json
import numpy as np, cv2

TILES = 3
BRIGHT = 0.5
SMOOTH = 0.05
NCLUST = 4
MINCL = 0.05
MINCAND = 50
TOL = 0.02
DARK = 0.05
GATE = 0.5


def load(d):
    m = {}
    for l in open(os.path.join(d, 'meta.txt')):
        k, v = l.split()
        m[k] = float(v)
    h, w = int(m['height']), int(m['width'])
    m['h'], m['w'] = h, w
    m['T'] = np.array([m['clip0'], m['clip1'], m['clip2']], np.float32)
    m['in'] = np.fromfile(os.path.join(d, 'in.f32'), np.float32).reshape(h, w, 4)[..., :3]
    m['state'] = np.fromfile(os.path.join(d, 'state.u8'), np.uint8).reshape(h, w)
    p = os.path.join(d, 'rho.f32')
    if os.path.exists(p) and os.path.getsize(p):
        m['rho'] = np.fromfile(p, np.float32).reshape(3, h, w).transpose(1, 2, 0)
    for k in ('donor', 'border'):
        p = os.path.join(d, k + '.u8')
        if os.path.exists(p) and os.path.getsize(p):
            m[k] = np.fromfile(p, np.uint8).reshape(h, w)
    return m


def pack(wd):
    """Packs the dumps of one image into pack.npz (float16 values, the flags as bytes):
    only what this script reads. Returns the path."""
    x7, x10, x9 = (load(os.path.join(wd, 'x%d' % x)) for x in (7, 10, 9))
    tru = load(os.path.join(wd, 'truth'))
    out = os.path.join(wd, 'pack.npz')
    np.savez(out, T=x7['T'], h=x7['h'], w=x7['w'], v=x7['in'].astype(np.float16), state=x7['state'],
             rho7=x7['rho'].astype(np.float16), rho10=x10['rho'].astype(np.float16),
             rho9=x9['rho'].astype(np.float16), donor=x10['donor'], border=x10['border'],
             Ttru=tru['T'], vtru=tru['in'].astype(np.float16), statetru=tru['state'])
    return out


def unpack(wd):
    z = np.load(os.path.join(wd, 'pack.npz'))
    f = lambda k: z[k].astype(np.float32)
    h, w = int(z['h']), int(z['w'])
    x7 = dict(T=z['T'], h=h, w=w, state=z['state'], rho=f('rho7'))
    x7['in'] = f('v')
    x10 = dict(rho=f('rho10'), donor=z['donor'], border=z['border'])
    x9 = dict(rho=f('rho9'))
    tru = dict(T=z['Ttru'], h=h, w=w, state=z['statetru'], **{'in': f('vtru')})
    return x7, x10, x9, tru


def angle(a, b):
    a = np.maximum(a, 0)
    b = np.maximum(b, 0)
    c = (a * b).sum(-1) / np.maximum(np.linalg.norm(a, axis=-1) * np.linalg.norm(b, axis=-1), 1e-12)
    return np.degrees(np.arccos(np.clip(c, -1, 1)))


def gd_weight(n):
    """_gd_weight() of gradient.c on a value normalized to its clipping level."""
    n = np.clip(n, 0, 1)
    x = np.where(n <= 0.65, n / 0.65, (1 - n) / 0.35)
    return x * x * (3 - 2 * x) + 1e-3


def implied(rho, v, state, T):
    """The initial guess of the channel solves in _gd_solve() for a flagged channel j:
    the mean of rho_j / rho_k * v_k over the unflagged k, weighted by _gd_weight(v_k /
    T_k), at least v_j, with the ratio of _gd_ratio(); unflagged channels keep v. Fully
    clipped pixels: rho itself, whose direction the hue fill keeps. Not the solve."""
    out = v.copy()
    flags = np.stack([(state >> c) & 1 for c in range(3)], -1).astype(bool)
    for j in range(3):
        num = np.zeros(v.shape[:2], np.float32)
        den = np.zeros(v.shape[:2], np.float32)
        for k in range(3):
            if k == j:
                continue
            ok = flags[..., j] & ~flags[..., k]
            wk = gd_weight(v[..., k] / T[k])
            ratio = np.maximum(rho[..., j], 0) / np.maximum(rho[..., k], 1e-4)
            num += np.where(ok, wk * ratio * v[..., k], 0)
            den += np.where(ok, wk, 0)
        est = np.where(den > 0, num / np.maximum(den, 1e-12), v[..., j])
        out[..., j] = np.where(flags[..., j], np.maximum(est, v[..., j]), out[..., j])
    full = flags.all(-1)
    out[full] = np.maximum(rho[full], 1e-4)
    return out


def truth_grid(clp, tru, k, kind):
    t = tru['in'] * (clp['T'] / tru['T']) * 2.0 ** k
    bad = cv2.dilate((tru['state'] > 0).astype(np.uint8), np.ones((3, 3), np.uint8)) > 0
    excl = bad.copy()
    shift = (0.0, 0.0)
    if kind == 'pair':
        lg = lambda a: np.log(np.maximum(a.sum(-1), 1e-6)).astype(np.float32)
        win = cv2.createHanningWindow((clp['w'], clp['h']), cv2.CV_32F)
        lc = lg(np.minimum(clp['in'], clp['T']))
        lt = lg(np.minimum(t, clp['T']))
        (sx, sy), _ = cv2.phaseCorrelate(lc.copy(), lt.copy(), win)
        A = np.float32([[1, 0, -sx], [0, 1, -sy]])
        t = np.stack([cv2.warpAffine(t[..., c], A, (clp['w'], clp['h']), flags=cv2.INTER_LINEAR,
                                     borderMode=cv2.BORDER_REPLICATE) for c in range(3)], -1)
        excl = cv2.warpAffine(excl.astype(np.float32), A, (clp['w'], clp['h']), borderValue=1) > 0.01
        cover = cv2.warpAffine(np.ones((clp['h'], clp['w']), np.float32), A, (clp['w'], clp['h']), borderValue=0)
        excl |= cover < 0.99
        lt = np.log(np.maximum(t.sum(-1), 1e-6)).astype(np.float32)
        gm = cv2.Sobel(lt, cv2.CV_32F, 1, 0) ** 2 + cv2.Sobel(lt, cv2.CV_32F, 0, 1) ** 2
        excl |= cv2.dilate((gm > np.percentile(gm, 90)).astype(np.uint8), np.ones((5, 5), np.uint8)) > 0
        shift = (float(sx), float(sy))
    return t, excl, shift


def tile_map(h, w):
    ty = np.minimum(np.arange(h) * TILES // h, TILES - 1)
    tx = np.minimum(np.arange(w) * TILES // w, TILES - 1)
    return ty[:, None] * TILES + tx[None, :]


def pools(v, state, T):
    h, w = state.shape
    tile = tile_map(h, w)
    n = v / T
    s = v.sum(-1)
    smooth = state == 0
    for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        sn = np.roll(s, (dy, dx), (0, 1))
        stn = np.roll(state, (dy, dx), (0, 1))
        smooth &= (stn == 0) & (np.abs(sn - s) <= SMOOTH * s)
    cand = smooth & (n.max(-1) >= BRIGHT) & (v.min(-1) > 0)
    cand[0, :] = cand[-1, :] = cand[:, 0] = cand[:, -1] = False
    rng = np.random.default_rng(0)
    P = {}
    for ty in range(TILES):
        for tx in range(TILES):
            c = v[cand & (tile == ty * TILES + tx)]
            if len(c) < MINCAND:
                P[(ty, tx)] = []
                continue
            if len(c) > 20000:
                c = c[rng.choice(len(c), 20000, replace=False)]
            f = np.stack([np.log(c[:, 0] / c[:, 1]), np.log(c[:, 2] / c[:, 1])], 1).astype(np.float32)
            kk = min(NCLUST, len(f))
            # cv2.kmeans draws from OpenCV's own generator: seed it per tile so that a
            # tile's clusters do not depend on what ran before
            cv2.setRNGSeed(1000 + ty * TILES + tx)
            _, lab, cen = cv2.kmeans(f, kk, None, (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 50, 1e-4),
                                     3, cv2.KMEANS_PP_CENTERS)
            lab = lab.ravel()
            out = []
            for i in range(kk):
                share = float((lab == i).mean())
                if share < MINCL:
                    continue
                hv = np.array([np.exp(cen[i, 0]), 1.0, np.exp(cen[i, 1])], np.float32)
                out.append((hv / hv.max(), share))
            P[(ty, tx)] = out
    return P


def choose(vp, sp, T, cands):
    """vp, sp: values and states of the piece's partly clipped pixels."""
    flags = np.stack([(sp >> c) & 1 for c in range(3)], -1).astype(bool)
    meas = ~flags & (vp >= DARK * T)
    two = meas.sum(1) >= 2
    best = None
    scores = []
    for h, share in cands:
        feas = np.ones(len(vp), bool)
        for k in range(3):
            for m in range(3):
                if k == m:
                    continue
                test = flags[:, k] & meas[:, m]
                feas &= ~test | (vp[:, m] * h[k] / h[m] >= T[k] * (1 - TOL))
        fs = float(feas.mean())
        mt = np.nan
        if two.any():
            errs = []
            for a, b in ((0, 1), (0, 2), (1, 2)):
                q = two & meas[:, a] & meas[:, b]
                if q.any():
                    errs.append(np.abs(np.log(vp[q, a] / vp[q, b]) - np.log(h[a] / h[b])))
            mt = float(np.concatenate(errs).mean())
        scores.append((fs, mt, share, h))
    fmax = max(s[0] for s in scores)
    ok = [s for s in scores if s[0] >= fmax - 0.05]
    if any(np.isfinite(s[1]) for s in ok):
        best = min((s for s in ok if np.isfinite(s[1])), key=lambda s: s[1])
    else:
        best = max(ok, key=lambda s: (s[0], s[2]))
    return best[3], best[0], best[1]


def rule_f(clp, d10):
    v, state, T = clp['in'], clp['state'], clp['T']
    h, w = state.shape
    P = pools(v, state, T)
    nreg, lab = cv2.connectedComponents((state > 0).astype(np.uint8), connectivity=4)
    tile = tile_map(h, w)
    key = lab.astype(np.int64) * (TILES * TILES) + tile
    hue = np.full((h, w, 3), np.nan, np.float32)
    info = []
    flat = key.ravel()
    U = np.nonzero(lab.ravel() > 0)[0]
    if not len(U):
        return hue, np.zeros((h, w), bool), []
    order = U[np.argsort(flat[U], kind='stable')]
    ks = flat[order]
    starts = np.r_[0, np.nonzero(np.diff(ks))[0] + 1, len(ks)]
    allc = [c for t in P.values() for c in t]
    for i in range(len(starts) - 1):
        px = order[starts[i]:starts[i + 1]]
        t = int(ks[starts[i]] % (TILES * TILES))
        tyy, txx = divmod(t, TILES)
        cands = P[(tyy, txx)]
        level = 0
        if not cands:
            cands = [c for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dy or dx)
                     for c in P.get((tyy + dy, txx + dx), [])]
            level = 1
        if not cands:
            cands, level = allc, 2
        sp = state.ravel()[px]
        part = (sp > 0) & (sp < 7)
        if not cands or not part.any():
            continue
        hv, fs, mt = choose(v.reshape(-1, 3)[px[part]], sp[part], T, cands)
        hue.reshape(-1, 3)[px] = hv
        info.append(dict(n=int(len(px)), level=level, feasible=fs, match=mt))
    # donor share of each region's border, for the gate
    gate = np.zeros((h, w), bool)
    if d10 is not None and 'donor' in d10 and 'border' in d10:
        b = d10['border'] > 0
        dn = d10['donor'] > 0
        # a border pixel counts once for each region it touches (4-neighbors, no wrap)
        lp = np.pad(lab, 1)
        pairs = []
        by, bx = np.nonzero(b)
        bi = by * w + bx
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            ln = lp[by + 1 + dy, bx + 1 + dx]
            q = ln > 0
            pairs.append(np.stack([ln[q].astype(np.int64), bi[q]], 1))
        pairs = np.unique(np.concatenate(pairs), axis=0)
        tot = np.bincount(pairs[:, 0], minlength=nreg).astype(float)
        don = np.bincount(pairs[:, 0], weights=dn.ravel()[pairs[:, 1]], minlength=nreg)
        share = np.where(tot > 0, don / np.maximum(tot, 1), 1.0)
        gate = (share[lab] < GATE) & (lab > 0)
    return hue, gate, info


def main():
    a = sys.argv[1:]
    if a[0] == '--pack':
        import shutil
        for wd in a[1:]:
            pack(wd)
            for x in ('x7', 'x10', 'x9', 'truth'):
                shutil.rmtree(os.path.join(wd, x))
            print('packed', wd, flush=True)
        return
    root, plan = a[0], json.load(open(a[1]))
    names = a[2]
    outp = a[a.index('-o') + 1] if '-o' in a else None
    sel = plan if names == 'all' else [e for e in plan if e['name'] in names.split(',')]
    res = {}
    for e in sel:
        wd = os.path.join(root, e['name'])
        if not os.path.exists(os.path.join(wd, 'pack.npz')):
            continue
        x7, x10, x9, tru = unpack(wd)
        t, excl, shift = truth_grid(x7, tru, e['k'], e['kind'])
        st, v, T = x7['state'], x7['in'], x7['T']
        hueF, gate, info = rule_f(x7, x10)
        est = {'measured (7)': x7['rho'], 'continuous (10)': x10['rho'], "opposed's (9)": x9['rho']}
        fF = np.where(np.isfinite(hueF), hueF, x10['rho'])
        est['F everywhere'] = fF
        est['F gated on 10'] = np.where(gate[..., None] & np.isfinite(hueF), hueF, x10['rho'])
        est['F gated on 7'] = np.where(gate[..., None] & np.isfinite(hueF), hueF, x7['rho'])
        # the model of value 7 sets the hue of pixels with one flagged channel from their
        # measured channels; F there replaces that information with a constant
        two = np.isin(st, (3, 5, 6, 7))
        est['F gated on 7, 2+ flags'] = np.where((gate & two)[..., None] & np.isfinite(hueF), hueF, x7['rho'])
        est['F gated on 10, 2+ flags'] = np.where((gate & two)[..., None] & np.isfinite(hueF), hueF, x10['rho'])
        r = dict(kind=e['kind'], shift=shift, gate_share=float(gate[st > 0].mean()) if (st > 0).any() else 0.0,
                 fallback=[sum(i['n'] for i in info if i['level'] == L) for L in range(3)])
        for mname, msk in (('partial', (st > 0) & (st < 7)), ('full', st == 7)):
            mk = msk & ~excl & np.isfinite(t).all(-1) & (t.min(-1) > 0)
            r['n_' + mname] = int(mk.sum())
            if mk.sum() < 50:
                continue
            for k, rho in est.items():
                if not np.isfinite(rho[mk]).all():
                    raise ValueError('%s: non-finite estimate %s' % (e['name'], k))
                r['rho|%s|%s' % (mname, k)] = float(angle(rho[mk], t[mk]).mean())
                r['implied|%s|%s' % (mname, k)] = float(angle(implied(rho, v, st, T)[mk], t[mk]).mean())
        res[e['name']] = r
        print(e['name'], json.dumps({k: (round(x, 2) if isinstance(x, float) else x) for k, x in r.items()}), flush=True)
    if outp:
        json.dump(res, open(outp, 'w'), indent=1)


if __name__ == '__main__':
    main()

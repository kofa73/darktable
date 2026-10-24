# border_check.py <workdir>...: cheap check for issue border-classification (freq-domain2.md, fix 1 and
# its "direct test"). Needs workdirs processed by pair_align.py (aligned truth in align/).
# Per clipped region (8-connected component of the clipped frame's flags, masks.npz F in any channel,
# at least MIN_PX pixels): border pixels are the ring 2 px outside the region (1 px would pick up
# demosaicing of the clipped photosites). Each border pixel is classified by the darkest pixel
# within 2 px of it outside the region, in the clipped frame's own render (none.pfm, clipping level
# of the clipped frame = 1): sky if >= SKY, occluder if < OCC, else mixed.
# Color = mean of rgb / Y (linear Rec2020), as u'v'. Compared with the true interior color: the
# aligned truth on the region's pixels that it records (pair_align.py's noTruth left out) and that
# are not dark (truth Y >= 0.05). Reported per region: u'v' distance to the truth of the sky border,
# the occluder border, the whole border (what an unweighted Laplace fill averages), and of the
# interiors of inpaint opposed and gradient domain. Writes border_check.json.
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2

MIN_PX = int(os.environ.get('BC_MIN_PX', '500'))
SKY, OCC = float(os.environ.get('BC_SKY', '0.5')), float(os.environ.get('BC_OCC', '0.15'))
METHODS = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')
BINS = [0, 0.05, 0.15, 0.3, 0.5, 0.75, 1e9]
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)


def mean_uv(rgb, msk):
    if not msk.any():
        return None
    x = np.maximum(rgb[msk], 0)
    y = np.maximum(x @ ev2.M[1], 1e-6)
    return ev2.uv((x / y[:, None]).mean(0))


def dist(a, b):
    return None if a is None or b is None else float(np.hypot(*(a - b)))


def main(wd):
    name = os.path.basename(wd.rstrip('/'))
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    kind = sc['ref'].split('/bracketed/')[1].split('/')[0] if '/bracketed/' in sc['ref'] else '?'
    s = 2.0 ** (sc['k'] - ev2.E0)
    al = os.path.join(wd, 'align')
    if not os.path.exists(os.path.join(al, 'al_0001.tif')):
        print('%s: not aligned' % name)
        return
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
    clip = np.load(os.path.join(wd, 'masks.npz'))['F'].astype(np.float32).max(2) > 0
    Y = ev2.lum(np.maximum(N, 0))
    Yt = ev2.lum(np.maximum(T, 0))
    n, lab, st, _ = cv2.connectedComponentsWithStats(clip.astype(np.uint8), 8)
    H, W = clip.shape
    regs = []
    for i in range(1, n):
        x, y, w, h, area = st[i]
        if area < MIN_PX:
            continue
        y0, y1, x0, x1 = max(0, y - 6), min(H, y + h + 6), max(0, x - 6), min(W, x + w + 6)
        sl = np.s_[y0:y1, x0:x1]
        r = (lab[sl] == i).astype(np.uint8)
        border = (cv2.dilate(r, disk(2)) > 0) & ~(cv2.dilate(r, disk(1)) > 0)
        # darkest pixel within 2 px, outside the region (region pixels count as bright)
        yo = np.where(r > 0, np.float32(1e3), Y[sl])
        dark = cv2.erode(yo, disk(2))
        sky, occ = border & (dark >= SKY), border & (dark < OCC)
        inner = (r > 0) & ~noTruth[sl] & (Yt[sl] >= 0.05)
        if inner.sum() < 50:
            continue
        tru = mean_uv(T[sl], inner)
        g = dict(id=i, px=int(area), bbox=[int(x), int(y), int(w), int(h)], border=int(border.sum()),
                 sky=int(sky.sum()), occ=int(occ.sum()), truth_px=int(inner.sum()),
                 d_sky=dist(mean_uv(N[sl], sky), tru), d_occ=dist(mean_uv(N[sl], occ), tru),
                 d_all=dist(mean_uv(N[sl], border), tru))
        for m in METHODS:
            g['d_' + m] = dist(mean_uv(imgs[m][sl], inner), tru)
        # finer split: border pixels by the darkest value beyond them, BINS edges
        g['bins'] = [[int((border & (dark >= a) & (dark < b)).sum()), dist(mean_uv(N[sl], border & (dark >= a) & (dark < b)), tru)]
                     for a, b in zip(BINS[:-1], BINS[1:])]
        regs.append(g)
    json.dump(dict(kind=kind, regions=regs), open(os.path.join(wd, 'border_check.json'), 'w'), indent=1)
    f = lambda v: '  -  ' if v is None else '%.3f' % v
    for g in regs:
        print('%-9s %-40s %7d px  border %5d sky %3.0f%% occ %3.0f%%  duv sky %s occ %s all %s  %s %s' % (
            kind, name[:40], g['px'], g['border'], 100 * g['sky'] / g['border'], 100 * g['occ'] / g['border'],
            f(g['d_sky']), f(g['d_occ']), f(g['d_all']),
            ' '.join('%s %s' % (m[:3], f(g['d_' + m])) for m in METHODS), ''), flush=True)
        if g['px'] >= 20000:
            print('    bins %s' % '  '.join('<%g: %d %s' % (b, c, f(d)) for b, (c, d) in zip(BINS[1:], g['bins'])), flush=True)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)

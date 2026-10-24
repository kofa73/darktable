# border_select.py <workdir>...: issue hue-from-selected-location. Per clipped region (as
# border_check.py: 8-connected clipped render pixels, at least MIN_PX, border = ring 2 px outside),
# does the color of one selected border location predict the true interior color better than the
# blend of the whole border?
# Weight of a border pixel, after segmentation based's _calc_weight() (segbased.c:108-119), on the
# luminance Y of the clipped frame's own render (clipping level 1): smoothness
# max(0, 1 - 10 sqrt(std of Y over the 5x5 window)) and brightness b = min(1, 3x3 mean of Y).
#   sel_s   the pixel of largest smoothness (what segbased.c computes: its brightness factor is
#           max(1, b^2) = 1 for real data, see /tmp/hl-article/HANDOFF.md:1011)
#   sel_sb  the pixel of largest smoothness * b^2 (what the code seems to intend)
#   top_sb  the mean color of the 2% of border pixels (at least 5) with the largest smoothness * b^2
# Color of a selection: mean of rgb / Y over its 5x5 window (or over the top pixels), as u'v'.
# Compared with: the whole border (blend), the border pixels with >= 0.75 of the clip level beyond
# them (border_check.py's bright bin), and the interiors of inpaint opposed and gradient domain.
# Truth interior color as border_check.py. Prints a summary over all regions.
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ev2
from border_check import mean_uv, dist, disk

MIN_PX = int(os.environ.get('BC_MIN_PX', '5000'))
METHODS = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')


def win_uv(rgb, y, x, r=2):
    p = np.maximum(rgb[max(0, y - r):y + r + 1, max(0, x - r):x + r + 1].reshape(-1, 3), 0)
    yy = np.maximum(p @ ev2.M[1], 1e-6)
    return ev2.uv((p / yy[:, None]).mean(0))


def main(wd, out):
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    kind = sc['ref'].split('/bracketed/')[1].split('/')[0]
    s = 2.0 ** (sc['k'] - ev2.E0)
    N = ev2.read_pfm(os.path.join(wd, 'none.pfm')).astype(np.float32) * s
    imgs = {m: ev2.read_pfm(os.path.join(wd, m + '.pfm')).astype(np.float32) * s for m in METHODS}
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
    Y = ev2.lum(np.maximum(N, 0))
    Yt = ev2.lum(np.maximum(T, 0))
    m1 = cv2.blur(Y, (5, 5)); m2 = cv2.blur(Y * Y, (5, 5))
    smooth = np.maximum(0, 1 - 10 * np.sqrt(np.sqrt(np.maximum(m2 - m1 * m1, 0))))
    bright = np.minimum(1, cv2.blur(Y, (3, 3)))
    wsb = smooth * bright ** 2
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
        inner = (r > 0) & ~noTruth[sl] & (Yt[sl] >= 0.05)
        if inner.sum() < 50 or border.sum() < 20:
            continue
        tru = mean_uv(T[sl], inner)
        by, bx = np.nonzero(border)
        g = dict(kind=kind, pair=os.path.basename(wd.rstrip('/')), px=int(area), border=int(border.sum()))
        ks = np.argmax(smooth[sl][by, bx]); ksb = np.argmax(wsb[sl][by, bx])
        g['sel_s'] = dist(win_uv(N, by[ks] + y0, bx[ks] + x0), tru)
        g['sel_sb'] = dist(win_uv(N, by[ksb] + y0, bx[ksb] + x0), tru)
        top = np.argsort(-wsb[sl][by, bx])[:max(5, len(by) // 50)]
        tm = np.zeros_like(border); tm[by[top], bx[top]] = True
        g['top_sb'] = dist(mean_uv(N[sl], tm), tru)
        g['blend'] = dist(mean_uv(N[sl], border), tru)
        dark = cv2.erode(np.where(r > 0, np.float32(1e3), Y[sl]), disk(2))
        g['bright'] = dist(mean_uv(N[sl], border & (dark >= 0.75)), tru) if (border & (dark >= 0.75)).sum() >= 20 else None
        for m in METHODS:
            g[m] = dist(mean_uv(imgs[m][sl], inner), tru)
        out.append(g)


if __name__ == '__main__':
    out = []
    for wd in sys.argv[1:]:
        main(wd, out)
    json.dump(out, open('border_select.json', 'w'), indent=1)
    keys = ['sel_s', 'sel_sb', 'top_sb', 'blend', 'bright'] + METHODS
    print('regions >= %d px: %d; median u\'v\' distance of each color source to the true interior color' % (MIN_PX, len(out)))
    for kind in ['all'] + sorted(set(g['kind'] for g in out)):
        v = [g for g in out if kind == 'all' or g['kind'] == kind]
        med = lambda k: np.median([g[k] for g in v if g[k] is not None]) if any(g[k] is not None for g in v) else float('nan')
        print('%-9s n %3d  ' % (kind, len(v)) + '  '.join('%s %.4f' % (k[:9], med(k)) for k in keys))
    v = [g for g in out if g['bright'] is not None]
    print('share of regions where the selection beats the blend: sel_s %.0f%%, sel_sb %.0f%%, top_sb %.0f%%; '
          'top_sb beats the bright border in %.0f%% of %d regions that have one' % (
              100 * np.mean([g['sel_s'] < g['blend'] for g in out]), 100 * np.mean([g['sel_sb'] < g['blend'] for g in out]),
              100 * np.mean([g['top_sb'] < g['blend'] for g in out]), 100 * np.mean([g['top_sb'] < g['bright'] for g in v]), len(v)))

# region_oracle.py <workdir>...: per-region choice between two methods (issue per-region-method-choice).
# Needs workdirs processed by pair_align.py (align/, pair_align.json) with the linear renders kept.
# Builds the same reference as pair_align.py's blend mode: the clipped frame's own render (none.pfm)
# where no method changed pixels, the aligned truth where any did (+2 px), and the composite's own
# pixels where the truth is clipped or missing.
# Regions: 8-connected components of that changed mask. Each region with at least MIN_PX pixels is
# scored locally for both methods: SSIMULACRA 2 on a crop (bounding box + MARGIN px, at least CROP px
# per side; outside the region the crop is the reference, so only this region differs), at 0 and
# -1 EV; the oracle takes the method with the higher sum.
# Smaller regions take the method that scores better on the whole image (sum of 0 and -1 EV).
# The composite (each region from its chosen method) is then scored on the whole image like
# pair_align.py (half resolution with PA_HALF=1). Writes oracle.json with the per-region choices,
# local scores and simple region features (pixels, bounding box, share of the 3 px outer ring at or
# above 0.9 of the clipping level in the clipped frame's own render: sky-like border) for rule fits.
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ev2
from ssim_eval import score
from lin_eval import to_srgb8

A, B = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')
HALF = os.environ.get('PA_HALF') == '1'
MIN_PX = int(os.environ.get('RO_MIN_PX', '200'))
MARGIN, CROP = 24, 96
# other method pairs than the default write oracle-<A>-<B>.json; RO_IN: pair_align.py output to read
SUFFIX = '' if (A, B) == ('opposed', 'gd-wb4x10') else '-%s-%s' % (A, B)
disk = lambda r: cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1,) * 2)


def bgr8(lin, g):
    return cv2.cvtColor(to_srgb8(lin * g), cv2.COLOR_RGB2BGR)


def full_score(ref, img):
    out = {}
    for off in (0, -1):
        a, b = bgr8(ref, 2.0 ** off), bgr8(img, 2.0 ** off)
        if HALF:
            a = cv2.resize(a, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA)
            b = cv2.resize(b, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA)
        out[off] = score(a, b)
    return out


def main(wd):
    name = os.path.basename(wd.rstrip('/'))
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    s = 2.0 ** (sc['k'] - ev2.E0)
    al = os.path.join(wd, 'align')
    if not os.path.exists(os.path.join(al, 'al_0001.tif')):
        print('%s: not aligned' % name)
        return
    imgs = {m: ev2.read_pfm(os.path.join(wd, m + '.pfm')).astype(np.float32) * s for m in (A, B)}
    N = ev2.read_pfm(os.path.join(wd, 'none.pfm')).astype(np.float32) * s
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
    rel = lambda x: np.abs(x - N).max(2) / np.maximum(N.max(2), 1e-3)
    changed = clip.copy()
    for img in imgs.values():
        changed |= rel(img) > 0.01
    changed = cv2.dilate(changed.astype(np.uint8), disk(2)) > 0
    base = N.copy(); base[changed] = T[changed]

    def ref_for(img):
        r = base.copy(); r[noTruth] = img[noTruth]
        return r

    pa = json.load(open(os.path.join(wd, os.environ.get('RO_IN', 'pair_align.json'))))
    glob_best = A if pa['%s +0' % A] + pa['%s -1' % A] >= pa['%s +0' % B] + pa['%s -1' % B] else B
    n, lab, st, _ = cv2.connectedComponentsWithStats(changed.astype(np.uint8), 8)
    H, W = clip.shape
    Nmax = N.max(2)
    regions, choice = [], np.zeros(n, np.uint8)   # 0 = A, 1 = B
    choice[:] = 0 if glob_best == A else 1
    for i in range(1, n):
        x, y, w, h, area = st[i]
        reg = dict(id=i, px=int(area), bbox=[int(x), int(y), int(w), int(h)],
                   clipped_px=int((clip & (lab == i)).sum()), truth_px=int(((lab == i) & ~noTruth).sum()))
        # sky-like border: share of the 3 px ring just outside the region that is itself near the clip
        y0, y1, x0, x1 = max(0, y - 4), min(H, y + h + 4), max(0, x - 4), min(W, x + w + 4)
        m = (lab[y0:y1, x0:x1] == i).astype(np.uint8)
        ring = (cv2.dilate(m, disk(3)) > 0) & (m == 0)
        reg['ring_near_clip'] = float((Nmax[y0:y1, x0:x1][ring] >= 0.9).mean()) if ring.any() else None
        if area >= MIN_PX and reg['truth_px'] > 0:
            cx, cy = x + w // 2, y + h // 2
            hw, hh = max(CROP // 2, w // 2 + MARGIN), max(CROP // 2, h // 2 + MARGIN)
            sl = np.s_[max(0, cy - hh):min(H, cy + hh), max(0, cx - hw):min(W, cx + hw)]
            inr = lab[sl] == i
            loc = {}
            for j, mth in enumerate((A, B)):
                img = imgs[mth]
                r = ref_for(img)[sl]
                # only this region differs from the reference within the crop
                c = r.copy(); c[inr] = img[sl][inr]
                loc[mth] = sum(score(bgr8(r, 2.0 ** off), bgr8(c, 2.0 ** off)) for off in (0, -1))
            reg['local'] = loc
            choice[i] = 0 if loc[A] >= loc[B] else 1
        reg['choice'] = (A, B)[choice[i]]
        regions.append(reg)
    pick = choice[lab].astype(bool)
    comp = imgs[A].copy(); comp[pick] = imgs[B][pick]
    res = {m: full_score(ref_for(imgs[m]), imgs[m]) for m in (A, B)}
    res['oracle'] = full_score(ref_for(comp), comp)
    scored = [r for r in regions if 'local' in r]
    out = dict(global_best=glob_best, scores={k: {str(o): v for o, v in d.items()} for k, d in res.items()},
               regions_scored=len(scored), regions=len(regions),
               scored_px_share=float(sum(r['px'] for r in scored) / max(1, sum(r['px'] for r in regions))),
               chose_B_px=int(pick[changed].sum()), changed_px=int(changed.sum()), region_list=regions)
    json.dump(out, open(os.path.join(wd, 'oracle%s.json' % SUFFIX), 'w'), indent=1)
    cv2.imwrite(os.path.join(wd, 'oracle_choice%s.png' % SUFFIX),
                np.where(changed[..., None], np.where(pick[..., None], [0, 0, 255], [255, 128, 0]), bgr8(N, 0.5)).astype(np.uint8))
    print('%-40s regions %4d (scored %3d, %3.0f%% of px)  %s px %3.0f%%  ' % (
        name[:40], len(regions), len(scored), 100 * out['scored_px_share'], B[:3], 100 * out['chose_B_px'] / max(1, out['changed_px']))
        + '  '.join('%+dEV opp %6.1f gd %6.1f orc %6.1f' % (o, res[A][o], res[B][o], res['oracle'][o]) for o in (0, -1)), flush=True)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)

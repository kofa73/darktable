# region_rule.py <workdir>...: issue selftest-method-choice. Like region_oracle.py (same reference,
# regions and scoring), but the choice is a rule: per region by its clipped pixels' share of the image
# (B = gradient domain for regions below RR_REGION shares, A elsewhere), and per image by the image's
# clipped share (B for the whole image below RR_IMAGE shares). Every threshold is scored on the whole
# image; writes rule-<A>-<B>.json.
# Derived from region_oracle.py (issue per-region-method-choice).
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

    n, lab, st, _ = cv2.connectedComponentsWithStats(changed.astype(np.uint8), 8)
    H, W = clip.shape
    npx = float(H * W)
    # clipped pixels per region, as a share of the image
    cshare = np.bincount(lab[clip], minlength=n).astype(np.float64) / npx
    image_share = float(clip.mean())
    res = {m: full_score(ref_for(imgs[m]), imgs[m]) for m in (A, B)}
    out = dict(image_clipped_share=image_share, regions=n - 1, scores={m: {str(o): v for o, v in res[m].items()} for m in (A, B)},
               region_rule={}, image_rule={})
    for t in REGION_T:
        pick = (cshare < t)[lab] & changed
        if not pick.any():
            r = res[A]
        elif pick[changed].all():
            r = res[B]
        else:
            comp = imgs[A].copy(); comp[pick] = imgs[B][pick]
            r = full_score(ref_for(comp), comp)
        out['region_rule'][str(t)] = dict(scores={str(o): v for o, v in r.items()}, B_px_share=float(pick[changed].mean()))
    for t in IMAGE_T:
        m = B if image_share < t else A
        out['image_rule'][str(t)] = dict(method=m, scores={str(o): v for o, v in res[m].items()})
    json.dump(out, open(os.path.join(wd, 'rule-%s-%s.json' % (A, B)), 'w'), indent=1)
    print('%-40s share %.4f regions %4d  %s' % (name[:40], image_share, n - 1, '  '.join(
        '%s %+.1f/%+.1f' % (t, out['region_rule'][str(t)]['scores']['0'] - res[A][0], out['region_rule'][str(t)]['scores']['-1'] - res[A][-1])
        for t in REGION_T)), flush=True)


REGION_T = [float(x) for x in os.environ.get('RR_REGION', '0.0005,0.001,0.002,0.005,0.01,0.02,0.05').split(',')]
IMAGE_T = [float(x) for x in os.environ.get('RR_IMAGE', '0.02,0.04,0.066,0.1').split(',')]

if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)

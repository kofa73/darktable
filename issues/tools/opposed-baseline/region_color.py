# region_color.py <workdir>...: mean color of the fully clipped region (masks.npz F min >= 0.5) per
# variant (SSIM_METHODS) and of the aligned truth frame where it recorded the pixel: chromaticity
# as CIE xy-free proxy, the Rec2020 rgb ratios r/g and b/g of the linear means, and the hue angle of
# the mean in Oklab-like opponent axes (a: r-g, b: (r+g)/2-b) for a quick "pinker / bluer" reading
import sys, os, json
import numpy as np, cv2
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
import ev2
M = os.environ['SSIM_METHODS'].split(',')
for wd in sys.argv[1:]:
    sc = json.load(open(os.path.join(wd, 'scores.json'))); s = 2.0 ** (sc['k'] - ev2.E0)
    F = np.load(os.path.join(wd, 'masks.npz'))['F']; full = F.min(2) >= 0.5
    al = os.path.join(wd, 'align')
    ta = cv2.imread(os.path.join(al, 'al_0001.tif'), cv2.IMREAD_UNCHANGED)
    mk = cv2.imread(os.path.join(al, [f for f in os.listdir(al) if f.startswith('mk_') and f.endswith('.tif')][0]), cv2.IMREAD_UNCHANGED)
    T = cv2.cvtColor(ta[..., :3].astype(np.float32), cv2.COLOR_BGR2RGB)
    Ra = mk[..., 0].astype(np.float32)
    if Ra.max() > 1.5: Ra = Ra / np.iinfo(mk.dtype).max
    rec = full & (Ra <= 0.02)
    print(os.path.basename(wd.rstrip('/')), 'fully clipped px', full.sum(), 'with truth', rec.sum())
    def show(name, img, m):
        v = img[m].mean(0)
        print('  %-12s r/g %.3f  b/g %.3f  Y %.3f' % (name, v[0] / v[1], v[2] / v[1], v @ np.array([0.2627, 0.678, 0.0593])))
    for k in M:
        show(k, ev2.read_pfm(os.path.join(wd, k + '.pfm')), rec)
    if rec.sum():
        show('truth', T * s if T.max() > 2 else T, rec)

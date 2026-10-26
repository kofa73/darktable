# compares RapidRAW's exports (exposure -2 EV, "basic" tone mapper, 16-bit TIFF) with darktable
# renders (linear Rec2020 PFM) and the truth frame. Every darktable image is mapped into the
# RapidRAW export's linear sRGB scale (per-channel gains fitted on unclipped mid-tones, which also
# absorbs global white balance and matrix differences) and shown through RapidRAW's own display
# curve (shader.wgsl:2029-2034), so the RapidRAW column is its TIFF unchanged.
import sys, os, json
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
from pfm import read_pfm
import numpy as np, cv2

R2020_TO_SRGB = np.array([[1.6605, -0.5876, -0.0728],
                          [-0.1246, 1.1329, -0.0083],
                          [-0.0182, -0.1006, 1.1187]], np.float32)

def basic(x):
    x = np.clip(x, 0, 1)
    s = np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)
    s = np.power(s, 1 / 1.1)
    return 0.25 * s + 0.75 * s * s * (3 - 2 * s)

_lut_x = np.linspace(0, 1, 65536, dtype=np.float64) ** 3
_lut_y = basic(_lut_x)
def basic_inv(y):
    return np.interp(y, _lut_y, _lut_x).astype(np.float32)

def lumY(rgb):
    return rgb @ np.array([0.2126, 0.7152, 0.0722], np.float32)

def shift(src, ref):
    a = np.log2(np.maximum(lumY(ref), 1e-4)).astype(np.float32)
    b = np.log2(np.maximum(lumY(src), 1e-4)).astype(np.float32)
    win = cv2.createHanningWindow(a.shape[::-1], cv2.CV_32F)
    (sx, sy), resp = cv2.phaseCorrelate(a, b, win)
    A = np.float32([[1, 0, -sx], [0, 1, -sy]])
    return cv2.warpAffine(src, A, src.shape[1::-1], flags=cv2.INTER_LINEAR,
                          borderMode=cv2.BORDER_REPLICATE), (sx, sy, resp)

def gains(src, ref):
    # per-channel ratio ref / src on pixels well inside the measured range of both
    ok = (src.max(2) < 0.5) & (src.min(2) > 0.01) & (ref.max(2) < 0.5) & (ref.min(2) > 0.01)
    return np.array([np.median(ref[..., c][ok] / src[..., c][ok]) for c in range(3)], np.float32), ok.mean()

PAIRS = {
    'laneway': ('/tmp/hl-controls/rr/laneway', '/tmp/hl-controls/rr/laneway/truth.pfm',
                '/tmp/gradient-domain-eval/iain/rr/Laneway bracket set clipped.tiff'),
}
for k, p, raw in [('g065', 'g065-sunset-field-0347-0345', 'colored/g065-sunset-field/rr/2025-06-11-20-07-18-P1070347'),
                  ('g213', 'g213-park-clouds-1-0852-0850', 'sky/g213-park-clouds-1/rr/2025-07-22-18-09-18-P1070852'),
                  ('g294', 'g294-corridor-wall-lamp-0122-0120', 'night/g294-corridor-wall-lamp/rr/2025-08-20-19-54-25-P1080122'),
                  ('t60', 't60-wall-lamp-1-0034-0030', 'indoor/t60-wall-lamp-1/rr/2026-10-16-06-27-49-P1060034')]:
    PAIRS[k] = ('/tmp/hl-controls/test/' + p, '/tmp/hl-rules-study/runs/allpairs/%s/truth.pfm' % p,
                '/tmp/gradient-domain-eval/bracketed/%s.tiff' % raw)

COLS = [('truth', None), ('opposed', 'c-def'), ('white', 'c-white'), ('ours', 'c-lrr'), ('RapidRAW', None)]
out = sys.argv[1] if len(sys.argv) > 1 else '/tmp/hl-controls/rr/out'
os.makedirs(out, exist_ok=True)
info = {}
for name, (wd, truth_path, tiff) in PAIRS.items():
    dt = {c: read_pfm(os.path.join(wd, m + '.pfm')) @ R2020_TO_SRGB.T for c, m in COLS if m}
    h, w = dt['opposed'].shape[:2]
    rr16 = cv2.cvtColor(cv2.imread(tiff, cv2.IMREAD_UNCHANGED), cv2.COLOR_BGR2RGB)
    rr = basic_inv(cv2.resize(rr16.astype(np.float32) / 65535, (w, h), interpolation=cv2.INTER_AREA))
    truth = read_pfm(truth_path) @ R2020_TO_SRGB.T
    # bring every darktable image into the RapidRAW scale
    g, share = gains(dt['opposed'], rr)
    dt = {c: v * g for c, v in dt.items()}
    rr, s_rr = shift(rr, dt['opposed'])
    truth, s_t = shift(truth, dt['opposed'])
    gt, share_t = gains(truth, dt['opposed'])
    truth = truth * gt
    imgs = {'truth': truth, **dt, 'RapidRAW': rr}
    disp = {c: (basic(v) * 255 + 0.5).astype(np.uint8) for c, v in imgs.items()}
    for c, v in disp.items():
        cv2.imwrite(os.path.join(out, '%s-%s.png' % (name, c)), cv2.cvtColor(v, cv2.COLOR_RGB2BGR))
    info[name] = dict(gain_dt_to_rr=g.tolist(), fit_share=float(share), shift_rr=s_rr,
                      gain_truth=gt.tolist(), shift_truth=s_t)
    print(name, json.dumps(info[name]), flush=True)
json.dump(info, open(os.path.join(out, 'info.json'), 'w'), indent=1)

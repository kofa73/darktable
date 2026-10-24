# lin_eval.py <workdir>...: Kofa's request: no tone mapper, no raised exposure. The linear Rec2020
# renders that ev2 keeps with EV2_KEEP_PFM (truth.pfm aligned by pair_shift as ev2 does) are scaled
# so that the clipping level of the clipped frame is display white (s = 2^(k - E0), ev2.py:349),
# darkened by OFF EV (0, -1, -2), converted to sRGB and cut at white, without any tone curve.
# Then, per method: SSIMULACRA 2 on a composite (method within 25 px of the clipped pixels, truth
# elsewhere; see ssim_eval.py) and "added color" (share of those pixels whose LCh chroma exceeds
# the truth's by > 8; see excess_chroma.py). Writes lin.json into each workdir.
import sys, os, json
import numpy as np, cv2, colour
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ev2
from ssim_eval import score

DIL, DC = 25, 8.0
METHODS = os.environ.get('SSIM_METHODS', 'opposed,gd-wb4x10').split(',')


def to_srgb8(lin):
    a = np.clip(np.maximum(lin, 0) @ ev2.TO_SRGB.T, 0, 1)
    s = np.where(a <= 0.0031308, 12.92 * a, 1.055 * a ** (1 / 2.4) - 0.055)
    return (s * 255 + 0.5).astype(np.uint8)


def chroma(srgb8):
    lab = colour.XYZ_to_Lab(colour.sRGB_to_XYZ(srgb8.astype(np.float32) / 255))
    return np.hypot(lab[..., 1], lab[..., 2])


def main(wd):
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    s = 2.0 ** (sc['k'] - ev2.E0)
    truth = ev2.read_pfm(os.path.join(wd, 'truth.pfm')).astype(np.float32)
    if sc['kind'] == 'pair':
        sx, sy = sc['pair_shift'][:2]
        A = np.float32([[1, 0, -sx], [0, 1, -sy]])
        truth = cv2.warpAffine(truth, A, truth.shape[1::-1], flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    mk = np.load(os.path.join(wd, 'masks.npz'))
    clip = (mk['F'].astype(np.float32) > 0).any(2) | mk['all']
    reg = cv2.dilate(clip.astype(np.uint8), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * DIL + 1,) * 2)) > 0
    imgs = {m: ev2.read_pfm(os.path.join(wd, m + '.pfm')).astype(np.float32) for m in METHODS}
    res = {}
    for off in (0, -1, -2):
        g = s * 2.0 ** off
        t8 = to_srgb8(truth * g); Ct = chroma(t8)
        cv2.imwrite(os.path.join(wd, 'lin%+d_truth.png' % off), cv2.cvtColor(t8, cv2.COLOR_RGB2BGR))
        for m, img in imgs.items():
            x8 = to_srgb8(img * g)
            cv2.imwrite(os.path.join(wd, 'lin%+d_%s.png' % (off, m)), cv2.cvtColor(x8, cv2.COLOR_RGB2BGR))
            comp = t8.copy(); comp[reg] = x8[reg]
            d = chroma(x8) - Ct
            res['%s %+d' % (m, off)] = dict(ssim2=score(cv2.cvtColor(t8, cv2.COLOR_RGB2BGR), cv2.cvtColor(comp, cv2.COLOR_RGB2BGR)),
                                            added=float((d[reg] > DC).mean()))
    json.dump(res, open(os.path.join(wd, 'lin.json'), 'w'), indent=1)
    print('%-28s ' % os.path.basename(wd.rstrip('/')) + '  '.join(
        '%+dEV opp %5.1f/%4.2f%% gd %5.1f/%4.2f%%' % (off, res['opposed %+d' % off]['ssim2'], 100 * res['opposed %+d' % off]['added'],
                                                     res['gd-wb4x10 %+d' % off]['ssim2'], 100 * res['gd-wb4x10 %+d' % off]['added'])
        for off in (0, -1, -2)), flush=True)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)

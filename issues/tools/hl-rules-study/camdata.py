# camdata.py <ev3 workdir>...: camera-space data of the clipped frame, registered onto the
# render as masks.npz is, for the region features of the rules study.
# Writes cam.npz: V (H, W, 3) float16, per 2x2 quad the mean of each color's photosites,
# black-subtracted, divided by the white level, scaled by the synthetic gain for synth
# images and clipped at 1.001 as the synth patch in rawprepare.c does, then multiplied by
# the as-shot white balance normalized to green. In that space the module's input is as
# shot balanced, neutral is (1, 1, 1) ("as shot to reference", _gd_white()), and channel c
# clips at T[c] = wb[c] / wb[G] (times 0.98 for the flag). T is stored too.
# The registration (flip, scale, shift) is the one ev2.py found, read from scores.json.
import sys, os, json
import numpy as np, cv2, rawpy
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
from bracket_scan import white_level
from ev2 import _dihedral, model


def quads(path, gain, synth):
    cam = model(path)
    with rawpy.imread(path) as r:
        a = r.raw_image_visible.astype(np.float32)
        cols = r.raw_colors_visible
        black = np.array(r.black_level_per_channel, np.float32)
        white = white_level(cam, r)
        wb = np.array(r.camera_whitebalance, np.float64)
        a = (a - black[cols]) / (white - black[cols]) * gain
        if synth:
            a = np.minimum(a, 1.001)
        h, w = (a.shape[0] // 2) * 2, (a.shape[1] // 2) * 2
        a, cols = a[:h, :w], cols[:h, :w]
        v = np.zeros((h // 2, w // 2, 3), np.float32)
        for dy in (0, 1):
            for dx in (0, 1):
                c = int(cols[dy, dx])
                ch = 1 if c == 3 else c
                v[..., ch] += a[dy::2, dx::2] * (0.5 if ch == 1 else 1.0)
    m = np.array([wb[0], wb[1], wb[2]]) / wb[1]
    return v * m.astype(np.float32), m


def main(wd):
    sc = json.load(open(os.path.join(wd, 'scores.json')))
    msk = np.load(os.path.join(wd, 'masks.npz'))
    H, W = msk['full'].shape
    synth = sc['kind'] == 'synth'
    v, m = quads(sc['clipped'], 2.0 ** sc['k'] if synth else 1.0, synth)
    reg = sc['registration']
    A = np.float32([[1, 0, reg['shift'][0]], [0, 1, reg['shift'][1]]])
    out = np.stack([cv2.warpAffine(cv2.resize(_dihedral(v[..., c], reg['transform']), (W, H), interpolation=cv2.INTER_AREA),
                                   A, (W, H), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
                    for c in range(3)], -1)
    np.savez_compressed(os.path.join(wd, 'cam.npz'), V=out.astype(np.float16), T=m.astype(np.float32))


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        main(wd)
        print('cam', wd, flush=True)

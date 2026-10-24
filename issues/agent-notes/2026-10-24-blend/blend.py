# blend.py <workdir>...: per-channel geometric blend of the inpaint opposed and gradient domain
# (gd-r-mg4) renders, linear Rec2020: out = opp^(1-t) * gd^t; where either value is not positive,
# the linear mix. Writes blend<100t>.pfm for t in BL_T (default 0.25,0.5). Offline test of
# issue opposed-hue-blend: the camera-space harness found the 0.25 blend best.
import sys, os
import numpy as np
sys.path.insert(0, '/tmp/claude-gd-artefacts/scripts')
from pfm import read_pfm
# BL_PARTIAL=1: weight t * (1 - F.min), F = the clipped frame's per-channel clipped share
# (masks.npz): fully clipped pixels stay inpaint opposed's; writes blend<100t>p.pfm
PARTIAL = os.environ.get('BL_PARTIAL') == '1'
TS = [float(t) for t in os.environ.get('BL_T', '0.25,0.5').split(',')]
B = os.environ.get('BL_B', 'gd-r-mg4')
# BL_A: the first method (default inpaint opposed); BL_NAME: output name prefix
A = os.environ.get('BL_A', 'opposed')
NAME = os.environ.get('BL_NAME', 'blend')


def write_pfm(path, a):
    h, w, _ = a.shape
    with open(path + '.tmp', 'wb') as f:
        f.write(b'PF\n%d %d\n-1.0\n' % (w, h))
        np.ascontiguousarray(a[::-1], '<f4').tofile(f)
    os.replace(path + '.tmp', path)


if __name__ == '__main__':
    for wd in sys.argv[1:]:
        a = read_pfm(os.path.join(wd, A + '.pfm')).astype(np.float64)
        b = read_pfm(os.path.join(wd, B + '.pfm')).astype(np.float64)
        pos = (a > 0) & (b > 0)
        la, lb = np.log(np.where(pos, a, 1.0)), np.log(np.where(pos, b, 1.0))
        g = 1.0
        if PARTIAL:
            F = np.load(os.path.join(wd, 'masks.npz'))['F'].astype(np.float64)
            g = (1.0 - F.min(axis=2))[..., None]
        for t in TS:
            tt = t * g
            out = np.where(pos, np.exp((1 - tt) * la + tt * lb), (1 - tt) * a + tt * b)
            write_pfm(os.path.join(wd, NAME + '%d%s.pfm' % (round(100 * t), 'p' if PARTIAL else '')), out.astype(np.float32))
        print(wd, 'ok', flush=True)

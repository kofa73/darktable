# loads an hlcolor TEMPDEBUG dump: in, state, filled, rho, cur before and after the joint scaling
import numpy as np
def load(path):
    with open(path, 'rb') as f:
        w, h = np.fromfile(f, np.int32, 2)
        n = int(w) * int(h)
        d = dict(w=int(w), h=int(h))
        d['inp'] = np.fromfile(f, np.float32, 4 * n).reshape(h, w, 4)[..., :3]
        d['st'] = np.fromfile(f, np.uint8, n).reshape(h, w)
        d['filled'] = np.fromfile(f, np.uint8, n).reshape(h, w)
        d['rho'] = np.fromfile(f, np.float32, 3 * n).reshape(3, h, w).transpose(1, 2, 0)
        d['pre'] = np.fromfile(f, np.float32, 4 * n).reshape(h, w, 4)[..., :3]
        d['post'] = np.fromfile(f, np.float32, 4 * n).reshape(h, w, 4)[..., :3]
        # dumps of the new method made with the current dump.patch end with 1 / clipping level per channel
        t = np.fromfile(f, np.float32, 4)
        if t.size == 4: d['inv_clip'] = t[:3]
    return d
